# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.media - backend for the Media tab: OFFLINE DLSS 5 processing of photos
# and videos (no game needed). No Tk imports here - the UI lives in
# ui_media.py; everything below runs on the m13 ActionWorker thread.
#
#   EngineSession  ctypes wrapper on production\runtime\layer\x64\
#                  d5c_chain.dll (clean C ABI, chain_c_api.h). Lazy init:
#                  dlss5_create takes seconds (weights upload), so the handle
#                  is created once and kept; knob pushes are cheap setters.
#   Ffmpeg         resolves ffmpeg/ffprobe: <bundle>\tools first, then PATH;
#                  can download a release zip into <bundle>\tools.
#   SavePolicy     output naming/mode (ask|always|subdir), never overwrites
#                  the source.
#   MediaQueue     the batch: photo and video items processed sequentially.
#
# The video pipe pipeline (VideoJob) lives in media_video.py - split for the
# ~600-line file discipline; it is imported lazily where needed.
# ctypes pattern follows dlss5\chain\test_framegen_transfer.py.
# ============================================================================
import ctypes
import os
import shutil
import threading
import urllib.request
import zipfile

from . import paths
from .media_img import PhotoIO, ImageError  # noqa: F401  (re-export)

PHOTO_EXTS = {".png", ".bmp", ".jpg", ".jpeg"}
VIDEO_EXTS = {".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v",
              ".ts", ".wmv"}


class MediaError(Exception):
    """User-facing failure; the UI renders .args[0]."""


def classify(path):
    """'photo' | 'video' | None by extension."""
    ext = os.path.splitext(path)[1].lower()
    if ext in PHOTO_EXTS:
        return "photo"
    if ext in VIDEO_EXTS:
        return "video"
    return None


def _chain_dll_candidates():
    """d5c_chain.dll locations, best first. The production bundle ships it at
    runtime\\layer\\x64 (next to the layer manifest + the *.spv set); dev
    falls back to the m11d/m11-layer build dirs. paths.find() returns the
    MANIFEST path for layer_x64, so the dll is its sibling."""
    dirs = []
    for key in ("layer_x64", "m12_proxy", "m11d_exe"):
        try:
            dirs.append(os.path.dirname(paths.find(key)))
        except Exception:
            pass
    dirs += [
        os.path.join(paths.REPO, "dlss5", "m11d", "build", "Release"),
        os.path.join(paths.REPO, "dlss5", "m11d", "build-nmake", "chain"),
        os.path.join(paths.REPO, "dlss5", "chain", "build", "Release"),
    ]
    seen, out = set(), []
    for d in dirs:
        dll = os.path.join(d, "d5c_chain.dll")
        if d and d not in seen and os.path.exists(dll):
            seen.add(d)
            out.append(dll)
    # prefer a dir that also holds the shader set the engine needs
    out.sort(key=lambda p: 0 if os.path.exists(
        os.path.join(os.path.dirname(p), "gemm.spv")) else 1)
    return out


def _weights_for(cfg):
    """Same discovery the launcher uses (gamelaunch is imported read-only:
    weights_path_for never mutates config)."""
    try:
        from . import gamelaunch
        return gamelaunch.weights_path_for(cfg)
    except Exception:
        d = paths.find("weights")
        try:
            names = [n for n in os.listdir(d) if n.endswith(".safetensors")]
        except OSError:
            return None
        return os.path.join(d, sorted(names)[0]) if names else None


# ----------------------------------------------------------------------------
# EngineSession - one lazy d5c_chain handle, knob setters, process()
# ----------------------------------------------------------------------------
class EngineSession:
    """Owns the in-process engine handle. Created lazily on the first real
    use (dlss5_create uploads ~325 MB of weights - seconds), kept for the
    session, destroyed by close(). Scale is pinned to 1.0: media is always
    native (owner requirement - no upscale/reconstruction for photos/videos).

    NOT thread-safe by design: every method is meant to run on the single
    m13 ActionWorker thread, which is exactly how ui_media drives it."""

    # ui slider name -> (dll export, value caster)
    KNOBS = {
        "intensity":       ("dlss5_set_intensity", float),
        "local_tone":      ("dlss5_set_local_tone", float),
        "local_structure": ("dlss5_set_local_structure", float),
        "skin_structure":  ("dlss5_set_skin_structure", float),
        "auto_mask":       ("dlss5_set_auto_mask",
                            lambda v: 1 if v else 0),
        "style_index":     ("dlss5_set_style_index", int),
        "color_strength":  ("dlss5_set_color_strength", float),
        "sharpness":       ("dlss5_set_sharpness", float),
        "gain":            ("dlss5_set_gain", float),
        "blend":           ("dlss5_set_blend", float),
    }

    def __init__(self, cfg=None, dll=None, dll_path=None, weights=None,
                 shader_dir=None):
        self.cfg = cfg
        self._dll = dll                    # injectable for tests
        self._dll_path = dll_path
        self._weights = weights
        self._shader_dir = shader_dir
        self._h = None
        self._frame = 0
        self._out = None                   # persistent out buffer
        self._pending = {}                 # knob dict awaiting the handle

    # ------------------------------------------------------------ setup ---
    def _load_dll(self):
        if self._dll is not None:
            return self._dll
        path = self._dll_path
        if not path:
            cands = _chain_dll_candidates()
            if not cands:
                raise MediaError("d5c_chain.dll not found")
            path = cands[0]
        if not os.path.exists(path):
            raise MediaError("d5c_chain.dll not found: %s" % path)
        self._dll = ctypes.CDLL(path)
        self._dll_path = path
        self._bind(self._dll)
        return self._dll

    @staticmethod
    def _bind(dll):
        """argtypes/restype per chain_c_api.h. Skipped for injected fakes
        (test doubles are plain objects, not CDLL)."""
        if not isinstance(dll, ctypes.CDLL):
            return
        p, f, i, u = (ctypes.c_void_p, ctypes.c_float,
                      ctypes.c_int, ctypes.c_uint32)
        dll.dlss5_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                     f, f, f]
        dll.dlss5_create.restype = p
        dll.dlss5_destroy.argtypes = [p]
        dll.dlss5_destroy.restype = None
        dll.dlss5_process_frame.argtypes = [p, u, u, p, p, ctypes.c_long]
        dll.dlss5_process_frame.restype = i
        for _k, (attr, cast) in EngineSession.KNOBS.items():
            fn = getattr(dll, attr, None)
            if fn is None:
                continue
            fn.restype = None
            fn.argtypes = [p, i] if cast is int or attr == "dlss5_set_auto_mask" \
                else [p, f]
        # temporal history reset (absent on pre-temporal dlls - guard)
        fn = getattr(dll, "dlss5_reset_history", None)
        if fn is not None:
            fn.restype = None
            fn.argtypes = [p]

    def _ensure(self):
        if self._h:
            return
        dll = self._load_dll()
        weights = self._weights or _weights_for(self.cfg)
        shdir = self._shader_dir or os.path.dirname(self._dll_path)
        w_b = os.fsencode(weights) if weights else None
        s_b = os.fsencode(shdir + os.sep) if shdir else None
        h = dll.dlss5_create(w_b, s_b, 1.0, 1.0, 1.0)
        if not h:
            raise MediaError("dlss5_create failed (weights/shaders?)")
        self._h = h
        self._frame = 0
        if self._pending:                  # replay stored knobs once live
            self._apply(self._pending)

    # ------------------------------------------------------------ knobs ---
    def apply_knobs(self, knobs):
        """Store + push the ui knob dict ({name: value}); unknown keys are
        ignored ('scale' is deliberately absent - pinned native). Safe to
        call before the engine exists: values replay at dlss5_create."""
        if knobs:
            self._pending.update(knobs)
        if self._h:
            self._apply(knobs or {})

    def _apply(self, knobs):
        dll = self._dll
        for key, val in knobs.items():
            entry = self.KNOBS.get(key)
            if not entry:
                continue
            attr, cast = entry
            fn = getattr(dll, attr, None)
            if fn is None:
                continue
            fn(self._h, cast(val))

    # ------------------------------------------------------------ frames --
    def process(self, bgra, w, h):
        """BGRA bytes in -> BGRA bytes out (new buffer, input untouched)."""
        n = w * h * 4
        if len(bgra) != n:
            raise MediaError("frame size mismatch: %d != %d" % (len(bgra), n))
        self._ensure()
        dll = self._dll
        inbuf = ctypes.create_string_buffer(bytes(bgra), n)
        if self._out is None or len(self._out.raw) != n:
            self._out = ctypes.create_string_buffer(n)
        in_p = ctypes.cast(inbuf, ctypes.c_void_p)
        out_p = ctypes.cast(self._out, ctypes.c_void_p)
        self._frame += 1
        ret = dll.dlss5_process_frame(self._h, w, h, in_p, out_p, self._frame)
        if ret != 0:
            raise MediaError("dlss5_process_frame rc=%d" % ret)
        return self._out.raw

    def reset_history(self):
        """Drop the engine's temporal anchor so the NEXT process() call
        takes the first-frame path. Photos and unrelated video sources must
        never share history - call once per item, not per frame."""
        if not self._h or self._dll is None:
            return
        fn = getattr(self._dll, "dlss5_reset_history", None)
        if fn is None:
            return                       # pre-temporal dll: nothing to reset
        try:
            fn(self._h)
        except Exception:
            pass

    @property
    def ready(self):
        return bool(self._h)

    def close(self):
        if self._h and self._dll is not None:
            try:
                self._dll.dlss5_destroy(self._h)
            except Exception:
                pass
        self._h = None
        self._out = None


# ----------------------------------------------------------------------------
# Ffmpeg - resolver + downloader
# ----------------------------------------------------------------------------
class Ffmpeg:
    """ffmpeg/ffprobe discovery:
      (a) <bundle>\\tools\\ffmpeg.exe   (production bundle dir; in the dev
          tree paths.BUNDLE is dlss5\\ so dlss5\\tools is tried first, then
          the repo-level tools\\)
      (b) PATH via shutil.which
      (c) None - the UI offers download() into the bundle tools dir."""

    URL = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"

    def __init__(self, tools_dir=None, which=None):
        self._tools = tools_dir or self.default_tools_dir()
        self._which = which or shutil.which
        self._ff = None
        self._fp = None

    @staticmethod
    def default_tools_dir():
        """<bundle>\\tools; in the dev tree prefer the repo tools\\ when it
        already exists so downloaded binaries land somewhere natural."""
        repo_tools = os.path.join(paths.REPO, "tools")
        if paths.REPO != paths.BUNDLE and os.path.isdir(repo_tools):
            return repo_tools
        return os.path.join(paths.BUNDLE, "tools")

    def tools_dir(self):
        return self._tools

    def _find(self, name):
        cands = [os.path.join(self._tools, name),
                 os.path.join(paths.BUNDLE, "tools", name),
                 os.path.join(paths.REPO, "tools", name)]
        for c in cands:
            if c and os.path.exists(c):
                return c
        return self._which(name) or self._which(os.path.splitext(name)[0])

    def resolve(self):
        """ffmpeg.exe path or None (memoized)."""
        if self._ff is None:
            self._ff = self._find("ffmpeg.exe") or ""
        return self._ff or None

    def ffprobe(self):
        if self._fp is None:
            self._fp = self._find("ffprobe.exe") or ""
        return self._fp or None

    def reset_cache(self):
        self._ff = self._fp = None

    def download(self, dest_dir=None, progress_cb=None):
        """Fetch the essentials zip, extract ffmpeg.exe+ffprobe.exe into
        dest_dir (default: the bundle tools dir). Runs on a worker thread.
        Returns the installed ffmpeg.exe path."""
        dest = dest_dir or self._tools
        os.makedirs(dest, exist_ok=True)
        tmp = os.path.join(dest, "_ffmpeg_dl.zip")
        try:
            req = urllib.request.Request(
                self.URL, headers={"User-Agent": "m13-media"})
            with urllib.request.urlopen(req, timeout=60) as r, \
                    open(tmp, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got = 0
                while True:
                    chunk = r.read(1 << 18)
                    if not chunk:
                        break
                    f.write(chunk)
                    got += len(chunk)
                    if progress_cb:
                        progress_cb(got, total)
            with zipfile.ZipFile(tmp) as z:
                picked = {}
                for name in z.namelist():
                    base = name.replace("/", "\\").rsplit("\\", 1)[-1]
                    if base in ("ffmpeg.exe", "ffprobe.exe") and \
                            base not in picked:
                        picked[base] = name
                if "ffmpeg.exe" not in picked:
                    raise MediaError("ffmpeg.exe missing in the zip")
                for base, member in picked.items():
                    with z.open(member) as src, \
                            open(os.path.join(dest, base), "wb") as out:
                        shutil.copyfileobj(src, out)
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass
        self.reset_cache()
        return os.path.join(dest, "ffmpeg.exe")


# ----------------------------------------------------------------------------
# SavePolicy - where outputs land and what they are called
# ----------------------------------------------------------------------------
class SavePolicy:
    """cfg keys: media_out_dir (fixed dir for mode 'always'; None = next to
    source), media_save_mode ('ask' | 'always' | 'subdir'), media_video_preset.
    Naming: <name>.dlss5.<ext> (videos always .mp4). Never overwrites the
    SOURCE; an existing OUTPUT is de-conflicted with -1/-2 suffixes."""

    MODES = ("ask", "always", "subdir")
    SUBDIR = "dlss5"

    def __init__(self, cfg=None, mode=None, out_dir=None):
        self.cfg = cfg
        if mode is None and cfg is not None:
            mode = cfg.get("media_save_mode", "subdir")
        if out_dir is None and cfg is not None:
            out_dir = cfg.get("media_out_dir")
        self.mode = mode if mode in self.MODES else "subdir"
        self.out_dir = out_dir or None

    @staticmethod
    def output_name(src, kind):
        base = os.path.splitext(os.path.basename(src))[0]
        if kind == "video":
            return base + ".dlss5.mp4"
        ext = os.path.splitext(src)[1].lower()
        return base + ".dlss5" + (ext if ext in (".png", ".bmp") else ".png")

    def dir_for(self, src, batch_dir=None):
        """Target directory per mode. 'ask' uses the batch_dir the UI
        prompted for; 'always' the configured out dir; 'subdir' a dlss5
        folder next to the source (the default)."""
        if self.mode == "ask":
            return batch_dir or os.path.join(os.path.dirname(src),
                                             self.SUBDIR)
        if self.mode == "always":
            return self.out_dir or os.path.join(os.path.dirname(src),
                                                self.SUBDIR)
        return os.path.join(os.path.dirname(src), self.SUBDIR)

    def resolve(self, src, kind, batch_dir=None, create=True):
        """Full output path for src; None means 'needs a prompt' (ask mode
        without a batch_dir)."""
        if self.mode == "ask" and batch_dir is None:
            return None
        d = self.dir_for(src, batch_dir)
        if create:
            os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, self.output_name(src, kind))
        if os.path.normcase(os.path.abspath(dst)) == \
                os.path.normcase(os.path.abspath(src)):
            raise MediaError("refusing to overwrite source: %s" % src)
        return self._unique(dst)

    @staticmethod
    def _unique(dst):
        if not os.path.exists(dst):
            return dst
        stem, ext = os.path.splitext(dst)
        for i in range(1, 1000):
            cand = "%s-%d%s" % (stem, i, ext)
            if not os.path.exists(cand):
                return cand
        raise MediaError("cannot find a free output name for %s" % dst)


# ----------------------------------------------------------------------------
# MediaQueue - the batch of items processed on the worker
# ----------------------------------------------------------------------------
class MediaItem:
    """One queued file. status: queued|working|done|error|skipped|cancelled."""
    def __init__(self, path, kind):
        self.path = path
        self.kind = kind                 # "photo" | "video"
        self.status = "queued"
        self.out_path = None
        self.error = None


class MediaQueue:
    """Sequential batch runner driven from a ctl.submit() job. cancel() sets
    the shared event; VideoJob checks it between frames and kills ffmpeg."""

    def __init__(self):
        self.items = []
        self._stop = threading.Event()

    # ------------------------------------------------------------ edits ---
    def add(self, path):
        path = os.path.abspath(path)
        kind = classify(path)
        if kind is None:
            return None
        for it in self.items:
            if os.path.normcase(it.path) == os.path.normcase(path):
                return it                # already queued
        item = MediaItem(path, kind)
        self.items.append(item)
        return item

    def add_many(self, paths_):
        n = 0
        for p in paths_:
            if self.add(p) is not None:
                n += 1
        return n

    def add_folder(self, folder):
        """Photos only: videos come from explicit file adds."""
        n = 0
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            return 0
        for name in names:
            p = os.path.join(folder, name)
            if os.path.isfile(p) and classify(p) == "photo":
                if self.add(p) is not None:
                    n += 1
        return n

    def remove(self, path):
        self.items = [it for it in self.items
                      if os.path.normcase(it.path) != os.path.normcase(path)]

    def clear(self):
        self.items = []

    # ------------------------------------------------------------ run -----
    def cancel(self):
        self._stop.set()

    def reset(self):
        self._stop.clear()

    @property
    def cancelled(self):
        return self._stop.is_set()

    def pending(self):
        return [it for it in self.items if it.status == "queued"]

    def run_all(self, engine, policy, ffm=None, knobs=None, batch_dir=None,
                progress_cb=None, item_cb=None, log=None):
        """Process every queued item in order. Runs on the worker thread;
        returns (ok, msg) like every ctl.submit job. The cancel event is
        NOT cleared here - the caller re-arms via reset() before queuing
        the run, so a cancel pressed beforehand still wins."""
        if knobs:
            engine.apply_knobs(knobs)
        log = log or (lambda m: None)
        n_done = n_err = 0
        for item in self.items:
            if self._stop.is_set():
                break
            if item.status == "done":
                continue
            item.status = "working"
            try:
                dst = policy.resolve(item.path, item.kind, batch_dir)
                if dst is None:
                    item.status = "skipped"
                    item.error = "save mode 'ask' needs a folder choice"
                elif item.kind == "photo":
                    self._run_photo(item, dst, engine, ffm)
                else:
                    self._run_video(item, dst, engine, ffm, progress_cb)
                if item.status == "working":
                    item.status = "done"
                    item.out_path = dst
            except (MediaError, ImageError, OSError) as e:
                item.status = "error"
                item.error = str(e)
            if item.status == "done":
                n_done += 1
            elif item.status == "error":
                n_err += 1
            if item_cb:
                item_cb(item)
        if self._stop.is_set():
            for item in self.items:
                if item.status in ("queued", "working"):
                    item.status = "cancelled"
            return False, "cancelled (%d done, %d failed)" % (n_done, n_err)
        return True, "%d done, %d failed" % (n_done, n_err)

    def _run_photo(self, item, dst, engine, ffm):
        # JPEG photos decode through the ffmpeg resolver; PNG/BMP never
        # touch it (PhotoIO decides by magic bytes itself).
        # Reset temporal history: an unrelated photo must not inherit the
        # previous item's anchor (shared engine, independent content).
        engine.reset_history()
        w, h, bgra = PhotoIO.load(item.path, ffm=ffm)
        out = engine.process(bgra, w, h)
        PhotoIO.save(dst, w, h, out)

    def _run_video(self, item, dst, engine, ffm, progress_cb):
        # lazy import: media_video imports MediaError from here, so a
        # top-level import would be a module cycle
        from .media_video import VideoJob
        ffmpeg, ffprobe = ffm.resolve() if ffm else None, \
            ffm.ffprobe() if ffm else None
        if not ffmpeg or not ffprobe:
            raise MediaError("ffmpeg not found")
        log_path = os.path.join(paths.find("logs"), "media_ffmpeg.log")
        # Reset once per video: frames within it share temporal history
        # (that is the point of the anchor), across videos it must not bleed.
        engine.reset_history()
        job = VideoJob(ffmpeg, ffprobe, item.path, dst,
                       engine, log_path=log_path)
        ok, msg = job.run(
            progress_cb=(lambda i, t: progress_cb(item, i, t))
            if progress_cb else None,
            cancel_ev=self._stop)
        if not ok:
            item.status = "cancelled" if self._stop.is_set() else "error"
            item.error = msg
