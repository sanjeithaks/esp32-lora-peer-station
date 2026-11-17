#!/usr/bin/env python3
"""Peer diagnostic: 5 x STATUS on both boards, optional bidirectional test.

Usage:
    python tests\\peer_diag.py --p1 COM4 --p2 COM6
    python tests\\peer_diag.py --p1 COM4 --p2 COM6 --test

Every value is read from the hardware. Nothing is simulated.
"""
from __future__ import annotations

import argparse
import re
import threading
import time

import serial


def open_port(port: str):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True
    s.rts = True
    s.open()
    s.dtr = True
    s.rts = True
    return s


def status_readings(port: str, count: int = 5):
    ser = open_port(port)
    buf: list[str] = []
    stop = threading.Event()

    def drain():
        while not stop.is_set():
            try:
                line = ser.readline()
            except Exception:
                break
            if line:
                buf.append(line.decode("utf-8", "replace").rstrip())

    threading.Thread(target=drain, daemon=True).start()
    time.sleep(2.0)
    ready = [l for l in buf if l.startswith("PEERREADY")]
    buf.clear()
    for _ in range(count):
        ser.write(b"STATUS\n")
        ser.flush()
        time.sleep(1.0)
    stop.set()
    ser.close()
    text = "\n".join(buf)
    versions = re.findall(r"SX1278 VERSION: (0x[0-9A-Fa-f]+) (\w+)", text)
    freqs = re.findall(r"FREQUENCY: (\d+) Hz", text)
    opmodes = re.findall(r"OPMODE: (0x[0-9A-Fa-f]+)", text)
    return ready[-1] if ready else "", versions, freqs, opmodes


def bidirectional(p1: str, p2: str, command: str, seconds: float = 9.0):
    """Send from p1 to p2 and from p2 to p1, print real stages/RX lines."""
    a = open_port(p1)
    b = open_port(p2)
    bufs = {"A": [], "B": []}
    stop = threading.Event()

    def drain(key, ser):
        while not stop.is_set():
            try:
                line = ser.readline()
            except Exception:
                break
            if line:
                bufs[key].append(line.decode("utf-8", "replace").rstrip())

    threading.Thread(target=drain, args=("A", a), daemon=True).start()
    threading.Thread(target=drain, args=("B", b), daemon=True).start()
    time.sleep(3.0)

    def send(src, dst, cmd):
        bufs[src].clear()
        bufs[dst].clear()
        (a if src == "A" else b).write((cmd + "\n").encode())
        (a if src == "A" else b).flush()
        time.sleep(seconds)
        print(f"{p1 if src == 'A' else p2} -> {p2 if dst == 'B' else p1}: {cmd}")
        for l in bufs[src]:
            if l.startswith("TXSTAGE") and ("ACK" in l or "DELIVERY" in l):
                print("   TX:", l.strip().encode("ascii", "replace").decode())
            elif l.startswith("ACKRXPACKET"):
                print("   TX:", l.strip().encode("ascii", "replace").decode())
        for l in bufs[dst]:
            if l.startswith("RXPKT") or l.startswith("ACKTX"):
                print("   RX:", l.strip().encode("ascii", "replace").decode())

    send("A", "B", command)
    send("B", "A", command)
    stop.set()
    a.close()
    b.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--p1", default="COM4")
    ap.add_argument("--p2", default="COM6")
    ap.add_argument("--test", action="store_true",
                    help="also send TEXT,DIAG TEST in both directions")
    args = ap.parse_args()

    for port in (args.p1, args.p2):
        ready, versions, freqs, opmodes = status_readings(port)
        print("=" * 60)
        print(port)
        print("  READY    :", (ready or "no PEERREADY").encode("ascii", "replace").decode())
        print("  versions :", versions)
        print("  freq     :", freqs)
        print("  opmode   :", opmodes)

    if args.test:
        bidirectional(args.p1, args.p2, "TEXT,DIAG TEST")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
