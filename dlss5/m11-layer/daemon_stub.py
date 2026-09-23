# M11 dummy daemon: TCP roundtrip stub for nr_layer_win.
# Reads header(16B: magic,w,h,fmt) + w*h*4 pixels, boosts green, replies.
# Proves the layer's write-back path before the real chain daemon exists.
import socket
import struct
import sys

import numpy as np

HOST, PORT = "127.0.0.1", 47990


def recvn(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("eof")
        buf += chunk
    return bytes(buf)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(4)
    print(f"stub daemon on {HOST}:{PORT}", flush=True)
    while True:
        conn, _ = srv.accept()
        try:
            head = recvn(conn, 16)
            magic, w, h, fmt = struct.unpack("<4I", head)
            if magic == 0x314E524E:  # masked frame: colour + mask bytes
                payload = recvn(conn, w * h * 5)
                px = np.frombuffer(payload[: w * h * 4], dtype=np.uint8).copy()
            else:
                px = np.frombuffer(recvn(conn, w * h * 4), dtype=np.uint8).copy()
            px = px.reshape(h, w, 4)
            in_mean = px[:, :, :3].mean(axis=(0, 1)).copy()
            g = px[:, :, 1].astype(np.uint16) + 80
            px[:, :, 1] = np.clip(g, 0, 255).astype(np.uint8)  # B,G,R,A order
            main.n = getattr(main, "n", 0) + 1
            if main.n % 60 == 1:
                print(f"frame {main.n}: in BGR {in_mean.round(1)} -> "
                      f"out BGR {px[:,:,:3].mean(axis=(0,1)).round(1)}", flush=True)
            conn.sendall(px.tobytes())
        except Exception as e:  # keep serving
            print("req err:", e, flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
