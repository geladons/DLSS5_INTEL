# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# videoin - uniform frame source for the autotest rig.
#
# Accepts either a video file (decoded via ffmpeg rawvideo bgr24 pipe, the
# same path gamerec/artifact_scan use) or a directory of PNG frames (fed to
# ffmpeg through a concat list, so arbitrary file names work).
#
# Also owns:
#   * ffmpeg/ffprobe discovery (production/tools first, PATH fallback)
#   * optional PNG sequence dump (--keep-frames) so a frames dir is kept
#   * full-res frame extraction for suspects/ (input seek + -frames:v)
# ============================================================================
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FFMPEG_CANDIDATES = [
    os.environ.get("FFMPEG", ""),
    os.path.expanduser(r"~\Desktop\production\tools\ffmpeg.exe"),
    os.path.join(ROOT, "production", "tools", "ffmpeg.exe"),
    "ffmpeg",
]
FFPROBE_CANDIDATES = [
    os.environ.get("FFPROBE", ""),
    os.path.expanduser(r"~\Desktop\production\tools\ffprobe.exe"),
    os.path.join(ROOT, "production", "tools", "ffprobe.exe"),
    "ffprobe",
]


def find_tool(candidates):
    for p in candidates:
        if os.path.exists(p):
            return p
    return candidates[-1]


def find_ffmpeg():
    return find_tool(FFMPEG_CANDIDATES)


def find_ffprobe():
    return find_tool(FFPROBE_CANDIDATES)


def probe(ff, video):
    """Return (w, h, fps, n_frames|None). Works on files; dirs return 0s."""
    err = subprocess.run([ff, "-hide_banner", "-i", video],
                         capture_output=True, text=True).stderr
    m = re.search(r"(\d{2,5})x(\d{2,5})", err)
    fm = re.search(r"(\d+(?:\.\d+)?) fps", err)
    nm = re.search(r"NUMBER_OF_FRAMES[^:]*: (\d+)", err)
    if not m:
        raise RuntimeError("cannot probe %s:\n%s" % (video, err[-800:]))
    return (int(m.group(1)), int(m.group(2)),
            float(fm.group(1)) if fm else 60.0,
            int(nm.group(1)) if nm else None)


class FrameSource:
    """Iterates (idx, uint8[h,w,3] bgr) frames from a video or a PNG dir."""

    def __init__(self, source, fps=60.0, ffmpeg=None):
        import numpy as np  # noqa: F401  (early numpy check)
        self.ff = ffmpeg or find_ffmpeg()
        self.source = source
        self.is_dir = os.path.isdir(source)
        self._tmp = None
        if self.is_dir:
            def _natkey(f):
                return [int(t) if t.isdigit() else t
                        for t in re.split(r"(\d+)", f)]
            self.pngs = sorted((f for f in os.listdir(source)
                                if f.lower().endswith(".png")),
                               key=_natkey)
            if not self.pngs:
                raise RuntimeError("no *.png frames in %s" % source)
            # concat list (absolute paths, natural order); -fps_mode
            # passthrough keeps every still - default vsync drops frames
            # whose intrinsic image2 duration overlaps the declared one.
            fd, lst = tempfile.mkstemp(suffix=".ffconcat",
                                       prefix="autotest_")
            with os.fdopen(fd, "w") as fp:
                fp.write("ffconcat version 1.0\n")
                for f in self.pngs:
                    p = os.path.join(source, f).replace("\\", "/")
                    fp.write("file '%s'\nduration %.6f\n"
                             % (p, 1.0 / fps))
                fp.write("file '%s'\n" % os.path.join(
                    source, self.pngs[-1]).replace("\\", "/"))
            self._tmp = lst
            self.src_arg = ["-f", "concat", "-safe", "0", "-i", lst,
                            "-fps_mode", "passthrough"]
            from pngio import read_png
            first = read_png(os.path.join(source, self.pngs[0]))
            self.h, self.w = first.shape[:2]
            self.fps = fps
            self.n_frames = len(self.pngs)
        else:
            self.src_arg = ["-i", source]
            w, h, fps_v, n = probe(self.ff, source)
            self.w, self.h, self.fps, self.n_frames = w, h, fps_v, n

    def frames(self, keep_dir=None):
        """Yield (idx, frame). If keep_dir is set, also tee every frame to
        keep_dir/f%06d.png (single decode pass; PNGs written via ffmpeg on
        a second small pass would double decode cost, so we write through
        our own png writer - cheap for suspects-sized runs but real work
        for long captures; hence off by default)."""
        import numpy as np
        from pngio import write_png
        sz = self.w * self.h * 3
        cmd = ([self.ff, "-hide_banner", "-loglevel", "error"]
               + self.src_arg +
               ["-f", "rawvideo", "-pix_fmt", "bgr24", "-"])
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL,
                             bufsize=sz * 4)
        i = 0
        try:
            while True:
                buf = _read_exact(p.stdout, sz)
                if buf is None:
                    break
                f = np.frombuffer(buf, dtype=np.uint8).reshape(
                    self.h, self.w, 3)
                if keep_dir:
                    write_png(os.path.join(keep_dir, "f%06d.png" % i), f)
                yield i, f
                i += 1
        finally:
            # generator may be abandoned mid-stream (early break, error):
            # always reap the ffmpeg child so it never orphans on a pipe
            try:
                p.stdout.close()
            except Exception:
                pass
            try:
                p.terminate()
            except Exception:
                pass
            try:
                p.wait(timeout=5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
            if self._tmp and os.path.exists(self._tmp):
                os.remove(self._tmp)

    def close(self):
        if self._tmp and os.path.exists(self._tmp):
            os.remove(self._tmp)
            self._tmp = None


def _read_exact(stream, n):
    """read() may return short buffers on pipes; loop until n or EOF."""
    chunks = []
    got = 0
    while got < n:
        b = stream.read(n - got)
        if not b:
            return None if got == 0 else None
        chunks.append(b)
        got += len(b)
    return b"".join(chunks)


def extract_frame(video, idx, fps, out_png, n_frames=1):
    """Pull full-res PNG(s) starting at frame idx. Input seeking is
    frame-accurate on modern ffmpeg and cheap at any timestamp."""
    ff = find_ffmpeg()
    t = max(0.0, (idx - 0.5) / fps)
    out_pat = out_png if n_frames == 1 else out_png.replace(
        ".png", "_%d.png")
    cmd = [ff, "-y", "-hide_banner", "-loglevel", "error",
           "-ss", "%.4f" % t, "-i", video,
           "-frames:v", str(n_frames), out_pat]
    subprocess.run(cmd, capture_output=True)
    if n_frames == 1:
        return out_png if os.path.exists(out_png) else None
    got = [out_png.replace(".png", "_%d.png" % k)
           for k in range(1, n_frames + 1)]
    return [g for g in got if os.path.exists(g)]
