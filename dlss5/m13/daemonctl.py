# ============================================================================
# m13.daemonctl - NRCT control-channel client for the m11d daemon.
#
# Protocol (see dlss5/m11d/main.cpp): one TCP connection per request to
# 127.0.0.1:47990, 16-byte header {magic='NRCT', cmd, payload, reserved}:
#   cmd 1 SETGAIN: payload = IEEE754 float bits -> reply {magic, ok, gain, frames}
#   cmd 2 STATUS:  no payload             -> reply {magic, ok, gain, frames}
# The reply carries the ACTIVE gain and the processed-frame counter, so the
# caller can confirm a live change without restarting anything.
#
# GOTCHA: 127.0.0.1:47990 is co-owned by Sunshine (wildcard listener, its
# web UI). A raw TCP connect SUCCEEDS even with m11d down - the probe is
# only trustworthy when the reply magic matches NRCT (see alive()).
# ============================================================================
import socket
import struct

MAGIC_CTRL = 0x5443524E          # "NRCT", little-endian uint32
CMD_SETGAIN = 1
CMD_STATUS = 2
CMD_SETBLEND = 3
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 47990


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
            raise DaemonError("reply magic 0x%08x != NRCT (Sunshine on 47990?)"
                              % magic)
        if not ok:
            raise DaemonError("daemon rejected control cmd %d" % cmd)
        gain = struct.unpack("<f", struct.pack("<I", gain_bits))[0]
        return gain, frames

    def status(self):
        """-> (gain, frames_processed). Raises DaemonError if no daemon."""
        return self._roundtrip(CMD_STATUS)

    def set_gain(self, gain):
        """Live gain update; applies to the next processed frame."""
        if not 0.0 <= gain <= 16.0:
            raise DaemonError("gain %.3f out of range 0..16" % gain)
        payload = struct.unpack("<I", struct.pack("<f", gain))[0]
        return self._roundtrip(CMD_SETGAIN, payload)

    def set_blend(self, blend):
        """Live blend (vendor mix factor 0..1) update; next processed frame."""
        if not 0.0 <= blend <= 1.0:
            raise DaemonError("blend %.3f out of range 0..1" % blend)
        payload = struct.unpack("<I", struct.pack("<f", blend))[0]
        return self._roundtrip(CMD_SETBLEND, payload)

    def alive(self):
        """True only when an NRCT-speaking m11d answers (Sunshine doesn't)."""
        try:
            self.status()
            return True
        except DaemonError:
            return False
