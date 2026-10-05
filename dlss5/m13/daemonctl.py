# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13.daemonctl - NRCT control-channel client for the m11d daemon.
#
# Protocol (see dlss5/m11d/main.cpp): one TCP connection per request to
# 127.0.0.1:48990, 16-byte header {magic='NRCT', cmd, payload, reserved}:
#   cmd 1 SETGAIN: payload = IEEE754 float bits -> reply {magic, ok, gain, frames}
#   cmd 2 STATUS:  no payload             -> reply {magic, ok, gain, frames}
# The reply carries the ACTIVE gain and the processed-frame counter, so the
# caller can confirm a live change without restarting anything.
#
# Sunshine owns 47990. A raw TCP connect to another endpoint can still succeed
# without m11d, so the probe trusts only a reply whose magic matches NRCT.
# ============================================================================
import os
import socket
import struct

MAGIC_CTRL = 0x5443524E          # "NRCT", little-endian uint32
MAGIC_PLAIN = 0x304E524E         # "NRN0" frame-request magic (see m11d)
CMD_SETGAIN = 1
CMD_STATUS = 2
CMD_SETBLEND = 3
CMD_SETSCALE = 4
CMD_SETFRAMEGEN = 5
CMD_SETTEMPORAL = 13         # temporal history feed on/off
CMD_SETFGMODE = 14           # FG algorithm: 0 off, 1 transfer, 2 anchor interp
CMD_SETLOCALTONE = 6
CMD_SETLOCALSTRUCTURE = 7
CMD_SETSKINSTRUCTURE = 8
CMD_SETAUTOMASK = 9
CMD_SETSTYLEINDEX = 10
CMD_SETCOLORSTRENGTH = 11
CMD_SETINTENSITY = 15        # Phase30 compose mix slot (distinct from gain)
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = int(os.environ.get("NR_LAYER_PORT") or os.environ.get("DLSS5_PORT") or 48990)


class DaemonError(Exception):
    pass


class M11dClient:
    """One-shot NRCT requests. Not thread-safe: create per call or guard."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def _roundtrip(self, cmd, payload=0):
        try:
            with socket.create_connection((self.host, self.port),
                                          timeout=self.timeout) as s:
                s.sendall(struct.pack("<4I", MAGIC_CTRL, cmd, payload, 0))
                rep = b""
                while len(rep) < 16:
                    chunk = s.recv(16 - len(rep))
                    if not chunk:
                        break
                    rep += chunk
        except OSError as e:
            raise DaemonError("no m11d reply on %s:%d (%s)"
                              % (self.host, self.port, e)) from e
        if len(rep) != 16:
            raise DaemonError("short/missing NRCT reply (%d bytes) - daemon down?"
                              % len(rep))
        magic, ok, gain_bits, frames = struct.unpack("<4I", rep)
        if magic != MAGIC_CTRL:
            raise DaemonError("reply magic 0x%08x != NRCT (wrong service on 48990?)"
                              % magic)
        if not ok:
            raise DaemonError("daemon rejected control cmd %d" % cmd)
        gain = struct.unpack("<f", struct.pack("<I", gain_bits))[0]
        return gain, frames

    def status(self):
        """-> (gain, frames_processed). Raises DaemonError if no daemon."""
        return self._roundtrip(CMD_STATUS)

    def set_gain(self, gain):
        """Live residual-gain update (engine headGain); next processed frame."""
        if not 0.0 <= gain <= 16.0:
            raise DaemonError("gain %.3f out of range 0..16" % gain)
        payload = struct.unpack("<I", struct.pack("<f", gain))[0]
        return self._roundtrip(CMD_SETGAIN, payload)

    def set_intensity(self, intensity):
        """Live Phase30 compose-mix update (pc.c.w slot); next frame."""
        if not 0.0 <= intensity <= 2.0:
            raise DaemonError("intensity %.3f out of range 0..2" % intensity)
        payload = struct.unpack("<I", struct.pack("<f", intensity))[0]
        return self._roundtrip(CMD_SETINTENSITY, payload)

    def set_blend(self, blend):
        """Live blend (vendor mix factor 0..1) update; next processed frame."""
        if not 0.0 <= blend <= 1.0:
            raise DaemonError("blend %.3f out of range 0..1" % blend)
        payload = struct.unpack("<I", struct.pack("<f", blend))[0]
        return self._roundtrip(CMD_SETBLEND, payload)

    def set_scale(self, scale):
        """Live scale update (0.25..1.0); triggers engine re-init with new extent."""
        if not 0.20 <= scale <= 1.5:
            raise DaemonError("scale %.2f out of range 0.20..1.5" % scale)
        payload = struct.unpack("<I", struct.pack("<f", float(scale)))[0]
        return self._roundtrip(CMD_SETSCALE, payload)

    def set_framegen(self, enable):
        """Live frame generation toggle; next processed frame."""
        return self._roundtrip(CMD_SETFRAMEGEN, 1 if enable else 0)

    def set_temporal(self, enable):
        """Temporal history feed on/off (feature channels 7-9); off kills
        the previous-output echo for trail/pump A/B tests."""
        return self._roundtrip(CMD_SETTEMPORAL, 1 if enable else 0)

    def set_fg_mode(self, mode):
        """FG algorithm select: 0 disables, 1 residual transfer (+0 latency),
        2 anchor-to-anchor interpolation (+1 frame, LS-class). The daemon
        resets its anchor state on a mode change."""
        return self._roundtrip(CMD_SETFGMODE, int(mode))

    def set_local_tone(self, val):
        payload = struct.unpack("<I", struct.pack("<f", float(val)))[0]
        return self._roundtrip(CMD_SETLOCALTONE, payload)

    def set_local_structure(self, val):
        payload = struct.unpack("<I", struct.pack("<f", float(val)))[0]
        return self._roundtrip(CMD_SETLOCALSTRUCTURE, payload)

    def set_skin_structure(self, val):
        payload = struct.unpack("<I", struct.pack("<f", float(val)))[0]
        return self._roundtrip(CMD_SETSKINSTRUCTURE, payload)

    def set_auto_mask(self, enable):
        return self._roundtrip(CMD_SETAUTOMASK, 1 if enable else 0)

    def set_style_index(self, idx):
        return self._roundtrip(CMD_SETSTYLEINDEX, int(idx))

    def set_color_strength(self, val):
        payload = struct.unpack("<I", struct.pack("<f", float(val)))[0]
        return self._roundtrip(CMD_SETCOLORSTRENGTH, payload)

    def alive(self):
        """True only when an NRCT-speaking m11d answers (Sunshine doesn't)."""
        try:
            self.status()
            return True
        except DaemonError:
            return False

    def warmup(self, w=1920, h=1080, timeout=120.0):
        """Drive one NRN0 frame through the daemon so its lazy engine init
        (seconds of weight load + pipeline build) happens NOW, not inside
        the game's first presents where it reads as a raw-frame burst.
        The engine binds the request extent, so callers should pass the
        extent the game will actually present at (launch-args -width/
        -height, else the desktop default); a mismatch just means the
        daemon re-inits once on the first real frame - no worse than
        today. Returns True on a full reply; False on any transport
        failure - callers treat it as a soft hint, never a blocker."""
        frame_len = w * h * 4
        try:
            with socket.create_connection((self.host, self.port),
                                          timeout=15.0) as s:
                s.settimeout(timeout)
                s.sendall(struct.pack("<4I", MAGIC_PLAIN, w, h, 44))
                s.sendall(b"\x80" * frame_len)
                got = 0
                while got < frame_len:
                    chunk = s.recv(min(65536, frame_len - got))
                    if not chunk:
                        break
                    got += len(chunk)
            return got == frame_len
        except OSError:
            return False
