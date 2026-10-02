# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.test_media - OFFLINE tests for media.py / media_img.py. No real
# d5c_chain.dll, ffmpeg or weights are touched: the dll is a FakeDll ctypes
# double, ffmpeg/ffprobe are FakePopen/FakeRunner doubles, images are tiny
# crafted byte strings.
#   python test_media.py
# ============================================================================
import ctypes
import io
import json
import os
import struct
import sys
import tempfile
import threading
import zlib
import binascii

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _DLSS5)

from m13 import media_img
from m13.media import (EngineSession, Ffmpeg, SavePolicy,
                       MediaQueue, MediaError)

_MEDIA_SUITE_RAN = False    # flipped only if every script check reaches EOM
from m13.media_video import VideoJob
from m13.media_img import (png_encode, png_decode, bmp_encode, bmp_decode,
                           PhotoIO, ImageError, _paeth)


# ============================================================================
# PNG codec
# ============================================================================
def _chunk(ctype, payload):
    return (struct.pack(">I", len(payload)) + ctype + payload +
            struct.pack(">I", binascii.crc32(ctype + payload) & 0xFFFFFFFF))


def _craft_png(w, h, bpp, ctype, filter_id, px):
    """Wrap known pixels into a PNG forcing one filter on every row.
    `px` is the RECONSTRUCTED image in PNG channel order (RGB/RGBA)."""
    stride = w * bpp
    raw = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        row = bytearray(px[y * stride:(y + 1) * stride])
        filt = bytearray(stride)
        for i in range(stride):
            left = row[i - bpp] if i >= bpp else 0
            up = prev[i]
            ul = prev[i - bpp] if i >= bpp else 0
            if filter_id == 0:
                pred = 0
            elif filter_id == 1:
                pred = left
            elif filter_id == 2:
                pred = up
            elif filter_id == 3:
                pred = (left + up) >> 1
            else:
                pred = _paeth(left, up, ul)
            filt[i] = (row[i] - pred) & 0xFF
        raw.append(filter_id)
        raw += filt
        prev = row
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)
    return (media_img.PNG_SIG + _chunk(b"IHDR", ihdr) +
            _chunk(b"IDAT", zlib.compress(bytes(raw))) +
            _chunk(b"IEND", b""))


def _bgra_to_png_order(w, h, bgra, bpp):
    """BGRA -> RGB(A) for comparison against crafted recon pixels."""
    out = bytearray(w * h * bpp)
    if bpp == 4:
        out[0::4] = bgra[2::4]
        out[1::4] = bgra[1::4]
        out[2::4] = bgra[0::4]
        out[3::4] = bgra[3::4]
    else:
        out[0::3] = bgra[2::4]
        out[1::3] = bgra[1::4]
        out[2::3] = bgra[0::4]
    return bytes(out)


def _img(w, h):
    """Deterministic non-trivial BGRA image."""
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            out[i] = (x * 37 + y * 11) & 0xFF
            out[i + 1] = (x * 5 + y * 53) & 0xFF
            out[i + 2] = (x * 91 + y * 7) & 0xFF
            out[i + 3] = 255
    return bytes(out)


print("=== Test A: PNG roundtrip + all 5 filters ===")
W, H = 9, 5
bgra = _img(W, H)
w, h, dec = png_decode(png_encode(W, H, bgra))
assert (w, h) == (W, H) and dec == bgra, "png roundtrip mismatch"

rgba = _bgra_to_png_order(W, H, bgra, 4)
for f in range(5):
    blob = _craft_png(W, H, 4, 6, f, rgba)
    w2, h2, dec2 = png_decode(blob)
    assert dec2 == bgra, "png filter %d decode mismatch" % f
print("  encode->decode identical; filters 0..4 all decode [PASS]")

# RGB (ctype 2) decode + pure-python fallback path
rgb = _bgra_to_png_order(W, H, bgra, 3)
w3, h3, dec3 = png_decode(_craft_png(W, H, 3, 2, 4, rgb))
exp = bytearray(bgra)
exp[3::4] = b"\xff" * (W * H)
assert dec3 == bytes(exp), "ctype 2 decode mismatch"
keep = media_img._np
try:
    media_img._np = None                       # force the stdlib path
    for f in range(5):
        w4, h4, d4 = png_decode(_craft_png(W, H, 4, 6, f, rgba))
        assert d4 == bgra, "pure-python filter %d mismatch" % f
finally:
    media_img._np = keep
print("  ctype 2 RGB + pure-python fallback identical [PASS]")


# ============================================================================
# BMP codec
# ============================================================================
print("\n=== Test B: BMP 24/32 load/save ===")
w, h, d = bmp_decode(bmp_encode(W, H, bgra))
assert (w, h) == (W, H) and d == bgra, "bmp32 roundtrip mismatch"

# craft a 24-bit BMP with row padding (width 9 -> 27 bytes -> pad to 28)
stride24 = ((W * 24 + 31) // 32) * 4
pix = bytearray()
for y in range(H - 1, -1, -1):               # bottom-up
    row24 = bytearray()
    for x in range(W):
        i = (y * W + x) * 4
        row24 += bgra[i:i + 3]               # BGR triplet = BGRA minus alpha
    pix += row24 + b"\x00" * (stride24 - W * 3)
fh = b"BM" + struct.pack("<IHHI", 54 + len(pix), 0, 0, 54)
ih = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(pix),
                 2835, 2835, 0, 0)
w2, h2, d2 = bmp_decode(bytes(fh + ih) + bytes(pix))
assert (w2, h2) == (W, H) and d2 == bgra, "bmp24 decode mismatch"
print("  bmp32 roundtrip + bmp24 padded rows [PASS]")


# ============================================================================
# EngineSession with a fake dll
# ============================================================================
print("\n=== Test C: EngineSession (fake dll) ===")


class FakeDll(object):
    """ctypes-double: records every call; process echoes input->output."""

    def __init__(self):
        self.calls = []

    def dlss5_create(self, w, s, scale, gain, blend):
        self.calls.append(("create", w, s, scale, gain, blend))
        return 4242

    def dlss5_destroy(self, h):
        self.calls.append(("destroy", h))

    def dlss5_process_frame(self, h, w, hh, in_p, out_p, idx):
        self.calls.append(("proc", w, hh, idx))
        ctypes.memmove(out_p, in_p, w * hh * 4)
        return 0

    def __getattr__(self, name):
        if name.startswith("dlss5_set_"):
            def setter(h, v, _n=name):
                self.calls.append((_n, v))
            return setter
        raise AttributeError(name)


fake = FakeDll()
eng = EngineSession(dll=fake, weights="W.weights", shader_dir="S")
eng.apply_knobs({"intensity": 1.5, "auto_mask": True, "style_index": 2})
assert not eng.ready and not any(c[0] == "create" for c in fake.calls), \
    "apply_knobs must not force engine init"
src = _img(4, 3)
out = eng.process(src, 4, 3)
out2 = eng.process(src, 4, 3)
assert out == src and out2 == src, "echo fake mismatch"
creates = [c for c in fake.calls if c[0] == "create"]
assert len(creates) == 1, "engine init must happen once, got %d" % len(creates)
c0 = creates[0]
assert c0[1] == b"W.weights" and c0[2].startswith(b"S"), \
    "weights/shader_dir not passed: %r" % (c0,)
assert c0[3] == 1.0, "media scale must be pinned to 1.0 (native)"
setmap = {c[0]: c[1] for c in fake.calls if c[0].startswith("dlss5_set_")}
assert setmap["dlss5_set_intensity"] == 1.5
assert setmap["dlss5_set_auto_mask"] == 1 and \
    isinstance(setmap["dlss5_set_auto_mask"], int)
assert setmap["dlss5_set_style_index"] == 2
assert [c[3] for c in fake.calls if c[0] == "proc"] == [1, 2], \
    "frame_index must increment"
eng.apply_knobs({"sharpness": 0.4, "blend": 0.5, "gain": 0.9,
                 "local_tone": 1.2, "local_structure": 1.3,
                 "skin_structure": -1.0, "color_strength": 1.7,
                 "unknown_knob": 42})
setmap = {c[0]: c[1] for c in fake.calls if c[0].startswith("dlss5_set_")}
expect = {"dlss5_set_sharpness": 0.4, "dlss5_set_blend": 0.5,
          "dlss5_set_gain": 0.9, "dlss5_set_local_tone": 1.2,
          "dlss5_set_local_structure": 1.3,
          "dlss5_set_skin_structure": -1.0,
          "dlss5_set_color_strength": 1.7}
for k, v in expect.items():
    assert setmap.get(k) == v, "%s -> %r (want %r)" % (k, setmap.get(k), v)
assert "unknown_knob" not in str(fake.calls)
try:
    eng.process(b"\x00" * 8, 4, 3)
    raise AssertionError("size mismatch not caught")
except MediaError:
    pass
eng.close()
eng.close()                                  # double close is a no-op
assert [c for c in fake.calls if c[0] == "destroy"] == [("destroy", 4242)]
print("  create-once, knob->setter mapping, echo process, close [PASS]")


# ============================================================================
# VideoJob with fake ffmpeg/ffprobe
# ============================================================================
print("\n=== Test D: VideoJob commands, run, cancel, grab_frame ===")

PROBE_JSON = json.dumps({
    "streams": [
        {"codec_type": "video", "width": 64, "height": 32,
         "avg_frame_rate": "30000/1001", "nb_frames": "2"},
        {"codec_type": "audio", "codec_name": "aac"}],
    "format": {"duration": "0.0667"}})


class _FakeRunner:
    def __init__(self, stdout=PROBE_JSON, rc=0):
        self.out = type("R", (), {"stdout": stdout, "stderr": "",
                                  "returncode": rc})()

    def __call__(self, cmd, **kw):
        return self.out


class _SinkIO(io.BytesIO):
    """Encoder stdin double: close() marks the pipe but keeps getvalue()
    readable so the test can inspect what was written."""

    def close(self):
        self.was_closed = True


class _FakeProc:
    def __init__(self, role, frames_blob=b""):
        self.role = role
        self.stdout = io.BytesIO(frames_blob) if role == "dec" else None
        self.stdin = _SinkIO() if role == "enc" else None
        self.killed = False
        self.returncode = 0

    def wait(self, timeout=None):
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9


class _FakePopen:
    """Records argv; returns the right fake for decoder vs encoder."""

    def __init__(self, frames_blob=b""):
        self.cmds = []
        self.procs = []
        self.frames_blob = frames_blob

    def __call__(self, cmd, **kw):
        self.cmds.append(list(cmd))
        role = "enc" if ("-i" in cmd and "-" in cmd and
                         cmd[-1] != "-") else "dec"
        p = _FakeProc(role, self.frames_blob)
        self.procs.append(p)
        return p


class _FakeEngine:
    def __init__(self):
        self.n = 0

    def process(self, bgra, w, h):
        self.n += 1
        return bytes(bgra)

    def apply_knobs(self, k):
        pass

    def reset_history(self):
        pass


info = VideoJob.parse_probe(json.loads(PROBE_JSON))
assert (info.w, info.h) == (64, 32) and info.has_audio
assert info.fps_str == "30000/1001" and abs(info.fps - 29.97) < 0.01
assert info.frames == 2

frames = _img(64, 32) + _img(64, 32)         # two raw frames on dec stdout
popen = _FakePopen(frames)
job = VideoJob("ff.exe", "fp.exe", "in.mkv", "out dlss5.mp4",
               _FakeEngine(), popen=popen, runner=_FakeRunner(),
               log_path=None)
got = []
ok, msg = job.run(progress_cb=lambda i, t: got.append((i, t)))
assert ok, "run failed: %s" % msg
assert job.engine.n == 2 and got == [(1, 2), (2, 2)]
dec_cmd, enc_cmd = popen.cmds[0], popen.cmds[1]
assert "-f" in dec_cmd and "rawvideo" in dec_cmd and dec_cmd[-1] == "-"
for a in ("-map", "0:v", "1:a?", "-c:a", "copy", "-c:v", "libx264",
          "-preset", "medium", "-crf", "18"):
    assert a in enc_cmd, "encoder cmd missing %s: %s" % (a, enc_cmd)
assert enc_cmd[enc_cmd.index("-r") + 1] == "30000/1001"
assert enc_cmd[enc_cmd.index("-s") + 1] == "64x32"
assert enc_cmd[-1] == "out dlss5.mp4"
assert len(popen.procs[1].stdin.getvalue()) == 2 * 64 * 32 * 4
print("  probe parse, dec/enc argv (-map/-r/-c:a), 2 frames piped [PASS]")

# cancel: the event is set before the loop -> kill + 'cancelled'
popen2 = _FakePopen(frames)
job2 = VideoJob("ff.exe", "fp.exe", "in.mp4", "o.mp4", _FakeEngine(),
                popen=popen2, runner=_FakeRunner())
ev = threading.Event()
ev.set()
ok, msg = job2.run(cancel_ev=ev)
assert not ok and msg == "cancelled"
assert all(p.killed for p in popen2.procs), "cancel must kill ffmpeg pipes"
print("  cancel kills both pipes and reports cancelled [PASS]")

# grab_frame returns frame 0
popen3 = _FakePopen(frames)
job3 = VideoJob("ff.exe", "fp.exe", "in.mp4", "-", _FakeEngine(),
                popen=popen3, runner=_FakeRunner())
w3, h3, f0 = job3.grab_frame(0)
assert (w3, h3) == (64, 32) and f0 == _img(64, 32)
assert popen3.procs[0].killed, "grab_frame must kill the decoder"
print("  grab_frame(0) decodes exactly one frame [PASS]")


# ============================================================================
# SavePolicy
# ============================================================================
print("\n=== Test E: SavePolicy naming / never-overwrite ===")


class _Cfg:
    def __init__(self, d=None):
        self._d = d or {}

    def get(self, key, default=None):
        return self._d.get(key, default)


with tempfile.TemporaryDirectory() as td:
    srcdir = os.path.join(td, "in")
    os.makedirs(srcdir)
    src = os.path.join(srcdir, "pic.png")
    with open(src, "wb") as f:
        f.write(b"x")

    pol = SavePolicy(_Cfg({"media_save_mode": "subdir"}))
    dst = pol.resolve(src, "photo")
    exp = os.path.join(srcdir, "dlss5", "pic.dlss5.png")
    assert dst == exp, "subdir naming: %s != %s" % (dst, exp)
    assert os.path.normcase(dst) != os.path.normcase(src)
    assert os.path.exists(src)

    pol = SavePolicy(_Cfg({"media_save_mode": "always",
                           "media_out_dir": os.path.join(td, "out")}))
    dst = pol.resolve(src, "photo")
    assert dst == os.path.join(td, "out", "pic.dlss5.png"), dst

    pol = SavePolicy(_Cfg({"media_save_mode": "ask"}))
    assert pol.resolve(src, "photo") is None, "ask w/o batch_dir must ask"
    dst = pol.resolve(src, "photo", batch_dir=os.path.join(td, "picked"))
    assert dst == os.path.join(td, "picked", "pic.dlss5.png")

    # video -> always .mp4; jpg photo -> .png (no jpeg encoder)
    assert SavePolicy.output_name("c:\\v\\clip.mkv", "video") == \
        "clip.dlss5.mp4"
    assert SavePolicy.output_name("x.jpg", "photo") == "x.dlss5.png"
    assert SavePolicy.output_name("x.bmp", "photo") == "x.dlss5.bmp"

    # collision -> -1 suffix, source still untouched
    pol = SavePolicy(mode="subdir")
    d1 = pol.resolve(src, "photo")
    with open(d1, "wb") as f:
        f.write(b"y")
    d2 = pol.resolve(src, "photo")
    assert d2.endswith("pic.dlss5-1.png"), d2
    assert open(src, "rb").read() == b"x", "source overwritten!"
    print("  subdir/always/ask modes, .dlss5 naming, unique, no-overwrite [PASS]")


# ============================================================================
# MediaQueue + Ffmpeg resolver + PhotoIO dispatch
# ============================================================================
print("\n=== Test F: MediaQueue, Ffmpeg resolve chain, JPEG guard ===")

with tempfile.TemporaryDirectory() as td:
    p1 = os.path.join(td, "a.png")
    p2 = os.path.join(td, "b.png")
    PhotoIO.save(p1, W, H, bgra)
    PhotoIO.save(p2, W, H, bgra)
    vid = os.path.join(td, "c.mp4")
    with open(vid, "wb") as f:
        f.write(b"not a real video")

    q = MediaQueue()
    assert q.add(p1).kind == "photo" and q.add(vid).kind == "video"
    assert q.add(os.path.join(td, "x.txt")) is None   # unknown ext skipped
    assert q.add(p1) is q.items[0], "dedupe by path"
    assert q.add_folder(td) == 2, "folder adds photos only"

    eng = _FakeEngine()
    pol = SavePolicy(mode="subdir")
    done_items = []
    ok, msg = q.run_all(eng, pol, ffm=None, knobs={"intensity": 1.0},
                        item_cb=done_items.append)
    # photos processed (echo engine -> identical bytes); video fails (no ffmpeg)
    by_path = {it.path: it for it in q.items}
    assert by_path[p1].status == "done" and by_path[p2].status == "done"
    out1 = os.path.join(td, "dlss5", "a.dlss5.png")
    assert os.path.exists(out1)
    assert png_decode(open(out1, "rb").read()) == (W, H, bgra)
    assert by_path[vid].status == "error" and "ffmpeg" in by_path[vid].error
    assert eng.n == 2, "engine processed photos only"
    print("  photos -> dlss5 subdir; video w/o ffmpeg errors cleanly [PASS]")

    # cancel before run -> everything cancelled
    q2 = MediaQueue()
    q2.add(p1)
    q2.cancel()
    ok, msg = q2.run_all(_FakeEngine(), pol)
    assert not ok and q2.items[0].status == "cancelled"
    print("  pre-set cancel event cancels the queue [PASS]")

# Ffmpeg resolution: bundle tools first, then PATH (injected which)
tools = tempfile.mkdtemp()
ff = Ffmpeg(tools_dir=tools, which=lambda n: None)
assert ff.resolve() is None, "no ffmpeg anywhere must give None"
fake_exe = os.path.join(tools, "ffmpeg.exe")
with open(fake_exe, "wb") as f:
    f.write(b"exe")
ff2 = Ffmpeg(tools_dir=tools, which=lambda n: None)
assert ff2.resolve() == fake_exe, "bundle tools ffmpeg not picked"
ff3 = Ffmpeg(tools_dir=os.path.join(tools, "nope"),
             which=lambda n: "/usr/bin/ffmpeg" if n == "ffmpeg" else None)
assert ff3.resolve() == "/usr/bin/ffmpeg", "PATH fallback failed"
print("  Ffmpeg chain: tools dir -> PATH -> None [PASS]")

# JPEG without ffmpeg -> clean error, never a crash
with tempfile.TemporaryDirectory() as td:
    j = os.path.join(td, "p.jpg")
    with open(j, "wb") as f:
        f.write(b"\xff\xd8\xff\xe0" + b"\x00" * 32)
    try:
        PhotoIO.load(j, ffm=None)
        raise AssertionError("jpeg without ffmpeg must raise")
    except ImageError as e:
        assert "ffmpeg" in str(e)
print("  JPEG without ffmpeg -> ImageError [PASS]")


# ============================================================================
# Preview helpers: view_sample / png_b64_rgb / preview-res + i18n + config
# (the zoomable Before/After viewer renders through view_sample; ui_media
# itself needs Tk and is exercised manually - the pure halves live here.
# NOTE: MediaTab._set_image changed contract from (label, png_b64) to
# (ZoomView, w, h, bgra) - nothing outside ui_media.py ever called it.)
# ============================================================================
print("\n=== Test G: view_sample, png_b64_rgb, preview_max, i18n keys ===")

from m13.media_img import (view_sample, png_b64_rgb, downscale_nearest)
from m13 import i18n
from m13.config import DEFAULTS
import base64

# gradient BGRA: B=10+x, G=20+y, R=30, A=255
GW, GH = 8, 6
gb = bytearray()
for _y in range(GH):
    for _x in range(GW):
        gb += bytes([10 + _x, 20 + _y, 30, 255])
gb = bytes(gb)

# 1:1 identity: zoom 1, centred, viewport == image size -> EXACT pixels
rgb = view_sample(GW, GH, gb, 1.0, 0.5, 0.5, GW, GH, bg=(1, 2, 3))
assert len(rgb) == GW * GH * 3
for _y in range(GH):
    for _x in range(GW):
        i = (_y * GW + _x) * 3
        assert rgb[i] == 30 and rgb[i + 1] == 20 + _y and \
            rgb[i + 2] == 10 + _x, "1:1 sample mismatch @%d,%d" % (_x, _y)

# zoomed 4x at the centre stays inside the image (no bg corners)
rgb2 = view_sample(GW, GH, gb, 4.0, 0.5, 0.5, GW, GH, bg=(1, 2, 3))
assert len(rgb2) == GW * GH * 3 and not rgb2.startswith(b"\x01\x02\x03" * 8)

# panned fully off-image -> solid background
rgb3 = view_sample(GW, GH, gb, 1.0, -5.0, -5.0, GW, GH, bg=(1, 2, 3))
assert rgb3 == bytes([1, 2, 3]) * (GW * GH)

# pure-python nearest fallback agrees on a 1:1 mapping
keep = media_img._np
try:
    media_img._np = None
    rgb4 = view_sample(GW, GH, gb, 1.0, 0.5, 0.5, GW, GH, bg=(1, 2, 3))
    assert rgb4 == rgb, "fallback view_sample != numpy at 1:1"
finally:
    media_img._np = keep
print("  view_sample: 1:1 exact, zoom stays in-image, pan-out = bg [PASS]")

# png_b64_rgb decodes back through the stdlib PNG decoder (ctype 2)
w2, h2, dec = png_decode(base64.b64decode(png_b64_rgb(GW, GH, rgb)))
assert (w2, h2) == (GW, GH)
assert dec[0::4] == rgb[2::3] and dec[1::4] == rgb[1::3] and \
    dec[2::4] == rgb[0::3] and dec[3::4] == b"\xff" * (GW * GH)
print("  png_b64_rgb -> png_decode roundtrip [PASS]")

# preview-res math: media_preview_max bounds the LONGEST dim (nearest
# integer step, aspect kept); 0 disables (full-res handled in ui_media)
w5, h5, small5 = downscale_nearest(64, 32, _img(64, 32), 16, 16)
assert (w5, h5) == (16, 8), (w5, h5)
w6, h6, _s = downscale_nearest(40, 22, _img(40, 22), 1024, 1024)
assert (w6, h6) == (40, 22)                      # smaller than max: untouched
assert DEFAULTS.get("media_preview_max") == 1024, \
    "media_preview_max default must be 1024"
print("  preview downscale bounded by media_preview_max=%d [PASS]"
      % DEFAULTS["media_preview_max"])

# every new i18n key resolves in BOTH languages (never falls back to key)
for _lang in ("en", "ru"):
    i18n.set_language(_lang)
    for _k in ("media_apply", "media_save", "media_saved",
               "media_save_photo_only", "media_save_wait",
               "media_processing", "media_preview_approx",
               "media_preview_size", "media_zoom_fit"):
        _t = i18n.tr(_k, 640, 480) if _k == "media_preview_approx" else \
            i18n.tr(_k, "X") if _k == "media_saved" else i18n.tr(_k)
        assert _t and _t != _k, "%s missing for %s" % (_k, _lang)
i18n.set_language("en")
print("  i18n keys resolve in en+ru [PASS]")

# ui_media module-level invariants (no Tk instantiated - import only)
from m13 import ui_media
assert ui_media.DEBOUNCE_MS == 600, "debounce must be ~600 ms"
assert hasattr(ui_media, "ZoomView") and hasattr(ui_media, "_ViewState")
assert ui_media._ViewState.REL_MIN == 0.25 and \
    ui_media._ViewState.REL_MAX == 8.0
print("  ui_media: DEBOUNCE_MS=600, ZoomView/_ViewState present [PASS]")

print("\nALL MEDIA TESTS PASSED!")

_MEDIA_SUITE_RAN = True


def test_media_suite():
    """pytest shim: this file is a script-style suite - all checks run at
    import/collection time (a failed assert becomes a collection error).
    Reaching this function means every section already passed."""
    assert _MEDIA_SUITE_RAN
