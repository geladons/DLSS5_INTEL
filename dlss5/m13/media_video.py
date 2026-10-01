# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.media_video - the video pipeline for the Media tab (split from
# media.py for the ~600-line file discipline): one video file through the
# DLSS 5 engine via ffmpeg/ffprobe subprocess pipes.
#
#   VideoInfo  parsed ffprobe facts (w/h/fps/duration/frames/audio flag)
#   VideoJob   decode pipe -> EngineSession.process per frame -> encode pipe.
#              subprocess.Popen / subprocess.run are injectable so tests
#              never touch a real ffmpeg. The frame loop is cancellable via
#              a threading.Event and never blocks the worker without
#              progress: stdout is read in bounded exact-size chunks and
#              ffmpeg stderr goes to a log file (or DEVNULL).
#
# Container rule (per spec): the output is always h264 mp4. A .mp4/.mkv
# source keeps its extension via SavePolicy naming only when mp4 - every
# other container (incl. webm, where libvpx would be the "correct" codec)
# is re-encoded to h264 and named .mp4.
# ============================================================================
import json
import os
import subprocess

from .media import MediaError  # noqa: F401  (re-exported for callers)

_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class VideoInfo:
    """Parsed ffprobe facts for one input."""
    def __init__(self, w, h, fps_str, fps, duration, frames, has_audio):
        self.w = w
        self.h = h
        self.fps_str = fps_str          # rational text for -r ("30000/1001")
        self.fps = fps                  # float seconds base
        self.duration = duration
        self.frames = frames            # best estimate, may be 0
        self.has_audio = has_audio


class VideoJob:
    """Decode -> EngineSession.process per frame -> encode h264 mp4.
    Owns both ffmpeg child processes while run() executes; cancel via the
    shared event kills the pipes (no half-written output kept)."""

    def __init__(self, ffmpeg, ffprobe, src, dst, engine,
                 popen=None, runner=None, log_path=None):
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe
        self.src = src
        self.dst = dst
        self.engine = engine
        self._popen = popen or subprocess.Popen
        self._runner = runner or subprocess.run
        self.log_path = log_path
        self.info = None
        self._procs = []                 # live pipes for cancel()

    # -------------------------------------------------------------- probe --
    def probe(self):
        cmd = [self.ffprobe, "-v", "quiet", "-print_format", "json",
               "-show_format", "-show_streams", self.src]
        out = self._runner(cmd, capture_output=True, text=True)
        if getattr(out, "returncode", 0) != 0:
            raise MediaError("ffprobe failed: %s"
                             % getattr(out, "stderr", ""))
        self.info = self.parse_probe(json.loads(out.stdout or "{}"))
        return self.info

    @staticmethod
    def parse_probe(doc):
        vstream = None
        has_audio = False
        for s in doc.get("streams", []):
            if s.get("codec_type") == "video" and vstream is None:
                vstream = s
            elif s.get("codec_type") == "audio":
                has_audio = True
        if vstream is None:
            raise MediaError("no video stream")
        w, h = int(vstream["width"]), int(vstream["height"])
        rate = vstream.get("avg_frame_rate") or \
            vstream.get("r_frame_rate") or "25/1"
        try:
            num, den = rate.split("/")
            fps = float(num) / float(den) if float(den) else 0.0
        except (ValueError, ZeroDivisionError):
            fps = 0.0
        if fps <= 0:
            rate, fps = "25/1", 25.0
        try:
            dur = float(vstream.get("duration")
                        or doc.get("format", {}).get("duration") or 0)
        except (ValueError, TypeError):
            dur = 0.0
        try:
            frames = int(vstream.get("nb_frames") or 0)
        except (ValueError, TypeError):
            frames = 0
        if not frames and dur > 0:
            frames = int(round(dur * fps))
        return VideoInfo(w, h, rate, fps, dur, frames, has_audio)

    # ------------------------------------------------------------- commands
    def decoder_cmd(self):
        """Raw BGRA frames on stdout, diagnostics on stderr only."""
        return [self.ffmpeg, "-nostdin", "-v", "error", "-i", self.src,
                "-f", "rawvideo", "-pix_fmt", "bgra", "-"]

    def encoder_cmd(self, info):
        """rawvideo stdin + the ORIGINAL file as audio map source.
        -map 1:a? is optional: silent sources simply get no audio track."""
        return [self.ffmpeg, "-y",
                "-f", "rawvideo", "-pix_fmt", "bgra",
                "-s", "%dx%d" % (info.w, info.h), "-r", info.fps_str,
                "-i", "-", "-i", self.src,
                "-map", "0:v", "-map", "1:a?",
                "-c:a", "copy", "-c:v", "libx264",
                "-preset", "medium", "-crf", "18", self.dst]

    # ------------------------------------------------------------- run ----
    @staticmethod
    def _read_exact(stream, n):
        """Bounded incremental read: exactly n bytes or None at EOF."""
        buf = bytearray()
        while len(buf) < n:
            chunk = stream.read(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return bytes(buf)

    def _err_dest(self):
        if self.log_path:
            try:
                os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
                return open(self.log_path, "ab")
            except OSError:
                pass
        return subprocess.DEVNULL

    def _kill_all(self):
        for p in self._procs:
            try:
                p.kill()
            except Exception:
                pass

    def run(self, progress_cb=None, cancel_ev=None):
        """Frame loop. progress_cb(idx, total). Returns (ok, msg)."""
        info = self.info or self.probe()
        frame_sz = info.w * info.h * 4
        err = self._err_dest()
        try:
            dec = self._popen(self.decoder_cmd(), stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=err,
                              creationflags=_NO_WINDOW)
            enc = self._popen(self.encoder_cmd(info), stdin=subprocess.PIPE,
                              stdout=subprocess.DEVNULL, stderr=err,
                              creationflags=_NO_WINDOW)
            self._procs = [dec, enc]
            idx = 0
            while True:
                if cancel_ev is not None and cancel_ev.is_set():
                    self._kill_all()
                    return False, "cancelled"
                frame = self._read_exact(dec.stdout, frame_sz)
                if frame is None:
                    break
                out = self.engine.process(frame, info.w, info.h)
                try:
                    enc.stdin.write(out)
                    enc.stdin.flush()
                except (BrokenPipeError, OSError):
                    return False, "encoder pipe broke"
                idx += 1
                if progress_cb:
                    progress_cb(idx, info.frames)
            try:
                enc.stdin.close()
            except (BrokenPipeError, OSError):
                pass
            rc_e = enc.wait()
            dec.wait()
            if cancel_ev is not None and cancel_ev.is_set():
                return False, "cancelled"
            if rc_e != 0:
                return False, "encoder rc=%s" % rc_e
            if idx == 0:
                return False, "no frames decoded"
            return True, "%d frames" % idx
        finally:
            self._kill_all()               # pipes die with the job
            if err not in (None, subprocess.DEVNULL):
                try:
                    err.close()
                except Exception:
                    pass
            if cancel_ev is not None and cancel_ev.is_set():
                try:
                    os.remove(self.dst)    # never leave a partial output
                except OSError:
                    pass

    def grab_frame(self, index=0):
        """Decode a single frame (default: frame 0) for the preview button.
        The decoder is killed right after the frame is read."""
        info = self.info or self.probe()
        frame_sz = info.w * info.h * 4
        err = self._err_dest()
        dec = self._popen(self.decoder_cmd(), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=err,
                          creationflags=_NO_WINDOW)
        self._procs = [dec]
        try:
            frame = None
            for _i in range(index + 1):
                frame = self._read_exact(dec.stdout, frame_sz)
                if frame is None:
                    break
            if frame is None:
                raise MediaError("no frame decoded")
            return info.w, info.h, frame
        finally:
            self._kill_all()
            if err not in (None, subprocess.DEVNULL):
                try:
                    err.close()
                except Exception:
                    pass
