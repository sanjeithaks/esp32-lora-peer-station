"""Parse the transmitter/receiver serial protocol into dashboard state.

Machine lines (all real, produced by the firmware):

  TXREADY,role=TRANSMITTER,port=COM4,freq=...,sf=...,sync=0x12,radio=OK,seq=...
  TXSTAT,seq=...,type=T|M,c1=...,c2=...,c3=...,c4=...,text=<hex|->,
         attempts=...,tx=OK|FAIL,ack=OK|TIMEOUT,ackrssi=...,acksnr=...,
         txok=...,txfail=...,ackok=...,ackto=...,ms=...
  RXREADY,role=RECEIVER,freq=...,sf=...,sync=0x12,radio=OK
  RXSTAT,seq=...,type=T|M,c1=...,c2=...,c3=...,c4=...,text=<text|->,
         rssi=...,snr=...,rxc=...,valid=...,invalid=...,dup=...,lost=...,
         ack=OK|FAIL,ackok=...,ackfail=...,dupflag=0|1,ms=...
  RXERR,kind=...,reason=...
  RXHEARTBEAT,rxc=...,valid=...,invalid=...,dup=...,lost=...,ackok=...,
         ackfail=...,rearm=...,radio=OK|FAIL,last_seq=...,age=...,ms=...
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field


def _int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return None


def _float(value: str | None) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def parse_kv(payload: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for part in payload.split(","):
        key, sep, value = part.partition("=")
        if sep:
            out[key.strip().lower()] = value.strip()
    return out


def hex_decode(text: str) -> str:
    if not text or text == "-":
        return ""
    try:
        return bytes.fromhex(text).decode("utf-8", errors="replace")
    except ValueError:
        return ""


@dataclass
class TxState:
    port: str | None = None
    connected: bool = False
    role_ok: bool = False
    radio_ok: bool | None = None
    freq: int | None = None
    sequence: int | None = None
    packet_type: str | None = None
    channels: list[int | None] = field(default_factory=lambda: [None] * 4)
    text: str = ""
    attempts: int = 0
    local_tx_ok: bool | None = None
    ack_ok: bool | None = None
    ack_rssi: int | None = None
    ack_snr: float | None = None
    tx_ok_count: int = 0
    tx_fail_count: int = 0
    ack_ok_count: int = 0
    ack_timeout_count: int = 0
    last_result: str = ""
    outgoing: str = ""
    stage: str = ""
    stage_seq: int | None = None
    last_seen: float = 0.0
    last_wall: str = ""
    connected_message: str = ""


@dataclass
class RxState:
    port: str | None = None
    connected: bool = False
    role_ok: bool = False
    radio_ok: bool | None = None
    freq: int | None = None
    sequence: int | None = None
    packet_type: str | None = None
    channels: list[int | None] = field(default_factory=lambda: [None] * 4)
    text: str = ""
    rssi: int | None = None
    snr: float | None = None
    received: int = 0
    valid: int = 0
    invalid: int = 0
    duplicates: int = 0
    lost: int = 0
    ack_ok: bool | None = None
    ack_sent: int = 0
    ack_failed: int = 0
    last_seen: float = 0.0
    last_packet_mono: float = 0.0
    last_wall: str = ""
    connected_message: str = ""


@dataclass
class PacketRow:
    wall_time: str
    sequence: int | None
    packet_type: str | None
    channels: list[int | None]
    text: str
    rssi: int | None
    snr: float | None
    ack: str


class GroundState:
    """Holds the latest TX/RX state and the received packet history."""

    STAGE_LABELS = {
        "STARTED": "TRANSMISSION STARTED",
        "TX_COMPLETE": "RADIO TX COMPLETE",
        "TX_FAILED": "RADIO TRANSMISSION FAILED",
        "WAITING_ACK": "WAITING FOR ACKNOWLEDGEMENT",
        "ACK_RECEIVED": "ACK RECEIVED",
        "ACK_TIMEOUT": "ACK TIMEOUT",
        "DELIVERY_CONFIRMED": "DELIVERY CONFIRMED",
        "DELIVERY_NOT_CONFIRMED": "DELIVERY NOT CONFIRMED",
        "TRANSMISSION_FAILED": "TRANSMISSION FAILED",
    }

    def __init__(self, history_limit: int = 200) -> None:
        self.tx = TxState()
        self.rx = RxState()
        self.history: deque[PacketRow] = deque(maxlen=history_limit)
        self.events: deque[tuple[str, str, str]] = deque(maxlen=300)  # ts, level, msg
        self.link = "WAITING"
        self.notes: list[str] = []
        self._last_rx_seq: int | None = None

    # ------------------------------------------------------------------ events
    def add_event(self, message: str, level: str = "info") -> None:
        stamp = time.strftime("%H:%M:%S")
        self.events.appendleft((stamp, level, message))

    def set_connection(self, role: str, connected: bool, message: str) -> None:
        target = self.tx if role == "TX" else self.rx
        target.connected = connected
        target.connected_message = message
        if connected:
            self.add_event(message, "ok")
        else:
            self.add_event(message, "err")
            if role == "TX":
                self.tx.role_ok = False
            else:
                self.rx.role_ok = False

    # ------------------------------------------------------------------ lines
    def feed(self, role: str, line: str) -> None:
        line = line.strip()
        if not line:
            return
        if line.startswith("TXREADY,"):
            kv = parse_kv(line.split(",", 1)[1])
            self.tx.role_ok = kv.get("role", "").upper() == "TRANSMITTER"
            self.tx.radio_ok = kv.get("radio") == "OK"
            self.tx.freq = _int(kv.get("freq"))
            self.tx.last_seen = time.monotonic()
            self.add_event("transmitter ready: " + line, "ok" if self.tx.role_ok else "warn")
            return
        if line.startswith("RXREADY,"):
            kv = parse_kv(line.split(",", 1)[1])
            self.rx.role_ok = kv.get("role", "").upper() == "RECEIVER"
            self.rx.radio_ok = kv.get("radio") == "OK"
            self.rx.freq = _int(kv.get("freq"))
            self.rx.last_seen = time.monotonic()
            self.add_event("receiver ready: " + line, "ok" if self.rx.role_ok else "warn")
            return
        if line.startswith("TXSTAT,"):
            self._txstat(parse_kv(line.split(",", 1)[1]), role)
            return
        if line.startswith("TXCMD,"):
            kv = parse_kv(line.split(",", 1)[1])
            self.tx.sequence = _int(kv.get("seq"))
            self.tx.stage_seq = self.tx.sequence
            self.tx.packet_type = kv.get("type")
            self.tx.channels = [_int(kv.get(f"c{i}")) for i in range(1, 5)]
            self.tx.outgoing = hex_decode(kv.get("cmd", ""))
            self.tx.stage = "COMMAND ACCEPTED"
            self.tx.last_seen = time.monotonic()
            self.add_event(
                f"TX seq {self.tx.sequence}: outgoing payload {self.tx.outgoing}", "info")
            return
        if line.startswith("TXSTAGE,"):
            kv = parse_kv(line.split(",", 1)[1])
            code = kv.get("stage", "").upper()
            label = self.STAGE_LABELS.get(code, code)
            self.tx.stage = label
            self.tx.stage_seq = _int(kv.get("seq"))
            self.tx.last_seen = time.monotonic()
            level = "err" if code in ("TX_FAILED", "TRANSMISSION_FAILED",
                                      "DELIVERY_NOT_CONFIRMED") else \
                "warn" if code in ("ACK_TIMEOUT", "WAITING_ACK") else "ok"
            self.add_event(f"TX seq {self.tx.stage_seq}: {label}", level)
            return
        if line.startswith("RXSTAGE,"):
            kv = parse_kv(line.split(",", 1)[1])
            code = kv.get("stage", "").upper()
            seq = _int(kv.get("seq"))
            if code == "VALID":
                self.add_event(
                    f"RX seq {seq}: packet valid ({kv.get('rssi')} dBm, "
                    f"{kv.get('snr')} dB)", "ok")
            elif code == "ACK_SENT":
                self.add_event(
                    f"RX seq {seq}: ACK sent {'OK' if kv.get('ack') == 'OK' else 'FAILED'}",
                    "ok" if kv.get("ack") == "OK" else "err")
            return
        if line.startswith("RXSTAT,"):
            self._rxstat(parse_kv(line.split(",", 1)[1]))
            return
        if line.startswith("RXHEARTBEAT,"):
            kv = parse_kv(line.split(",", 1)[1])
            self.rx.radio_ok = kv.get("radio") == "OK"
            self.rx.received = _int(kv.get("rxc")) or self.rx.received
            self.rx.valid = _int(kv.get("valid")) or self.rx.valid
            self.rx.invalid = _int(kv.get("invalid")) or 0
            self.rx.duplicates = _int(kv.get("dup")) or 0
            self.rx.lost = _int(kv.get("lost")) or 0
            self.rx.ack_sent = _int(kv.get("ackok")) or self.rx.ack_sent
            self.rx.ack_failed = _int(kv.get("ackfail")) or self.rx.ack_failed
            return
        if line.startswith("RXERR,"):
            kv = parse_kv(line.split(",", 1)[1])
            kind = kv.get("kind", "?")
            reason = kv.get("reason", "")
            self.add_event(f"receiver error [{kind}] {reason}".strip(), "err")
            if kind == "RADIO":
                self.rx.radio_ok = False
            if kind == "INVALID":
                self.rx.invalid += 1
            return
        if line.startswith("ROLE="):
            if "TRANSMITTER" in line.upper():
                self.tx.role_ok = True
            elif "RECEIVER" in line.upper():
                self.rx.role_ok = True
            return
        if "RADIO=OK" in line.upper():
            (self.tx if role == "TX" else self.rx).radio_ok = True
            return
        if "RADIO=FAIL" in line.upper():
            (self.tx if role == "TX" else self.rx).radio_ok = False
            self.add_event(f"{'transmitter' if role == 'TX' else 'receiver'} radio failure",
                           "err")
            return
        if line.startswith(("PACKET INVALID", "TX: ", "LoRa init", "  attempt",
                            "RADIO HEALTH", "COMMUNICATION TIMEOUT", "RADIO LOST")):
            level = "err" if ("FAIL" in line.upper() or "INVALID" in line.upper()
                              or "TIMEOUT" in line.upper()) else "warn"
            self.add_event(line, level)
            return

    def _txstat(self, kv: dict[str, str], role: str) -> None:
        self.tx.role_ok = True
        self.tx.sequence = _int(kv.get("seq"))
        self.tx.packet_type = kv.get("type")
        self.tx.channels = [_int(kv.get(f"c{i}")) for i in range(1, 5)]
        self.tx.text = hex_decode(kv.get("text", ""))
        self.tx.attempts = _int(kv.get("attempts")) or 0
        self.tx.local_tx_ok = kv.get("tx") == "OK"
        self.tx.ack_ok = kv.get("ack") == "OK"
        self.tx.ack_rssi = _int(kv.get("ackrssi"))
        self.tx.ack_snr = _float(kv.get("acksnr"))
        self.tx.tx_ok_count = _int(kv.get("txok")) or 0
        self.tx.tx_fail_count = _int(kv.get("txfail")) or 0
        self.tx.ack_ok_count = _int(kv.get("ackok")) or 0
        self.tx.ack_timeout_count = _int(kv.get("ackto")) or 0
        self.tx.last_seen = time.monotonic()
        self.tx.last_wall = time.strftime("%H:%M:%S")

        if self.tx.ack_ok:
            self.tx.last_result = "DELIVERY CONFIRMED"
            self.add_event(
                f"TX seq {self.tx.sequence}: ACK received "
                f"(attempt {self.tx.attempts}, {self.tx.ack_rssi} dBm)", "ok")
        elif self.tx.local_tx_ok:
            self.tx.last_result = "TX DONE / ACK TIMEOUT"
            self.add_event(
                f"TX seq {self.tx.sequence}: local TX OK, ACK timeout", "warn")
        else:
            self.tx.last_result = "TRANSMISSION FAILED"
            self.add_event(f"TX seq {self.tx.sequence}: transmission failed", "err")

    def _rxstat(self, kv: dict[str, str]) -> None:
        self.rx.role_ok = True
        self.rx.sequence = _int(kv.get("seq"))
        self.rx.packet_type = kv.get("type")
        self.rx.channels = [_int(kv.get(f"c{i}")) for i in range(1, 5)]
        text = kv.get("text", "-")
        self.rx.text = "" if text == "-" else text
        self.rx.rssi = _int(kv.get("rssi"))
        self.rx.snr = _float(kv.get("snr"))
        self.rx.received = _int(kv.get("rxc")) or self.rx.received
        self.rx.valid = _int(kv.get("valid")) or self.rx.valid
        self.rx.invalid = _int(kv.get("invalid")) or 0
        self.rx.duplicates = _int(kv.get("dup")) or 0
        self.rx.lost = _int(kv.get("lost")) or 0
        self.rx.ack_ok = kv.get("ack") == "OK"
        self.rx.ack_sent = _int(kv.get("ackok")) or self.rx.ack_sent
        self.rx.ack_failed = _int(kv.get("ackfail")) or self.rx.ack_failed
        self.rx.last_seen = time.monotonic()
        self.rx.last_packet_mono = self.rx.last_seen
        self.rx.last_wall = time.strftime("%H:%M:%S")

        duplicate = kv.get("dupflag") == "1"
        if not duplicate and self.rx.sequence is not None:
            self.history.appendleft(PacketRow(
                wall_time=time.strftime("%H:%M:%S"),
                sequence=self.rx.sequence,
                packet_type=self.rx.packet_type,
                channels=list(self.rx.channels),
                text=self.rx.text,
                rssi=self.rx.rssi,
                snr=self.rx.snr,
                ack="OK" if self.rx.ack_ok else "FAILED",
            ))
            self.add_event(
                f"RX seq {self.rx.sequence}: "
                + ", ".join(str(c) for c in self.rx.channels)
                + f" | {self.rx.rssi} dBm / {self.rx.snr} dB | ACK "
                + ("OK" if self.rx.ack_ok else "FAILED"), "ok")

    # ------------------------------------------------------------------ view
    def update_link(self) -> None:
        now = time.monotonic()
        tx_age = now - self.tx.last_seen if self.tx.last_seen else None
        rx_age = now - self.rx.last_seen if self.rx.last_seen else None
        tx_fresh = tx_age is not None and tx_age < 15.0
        rx_fresh = rx_age is not None and rx_age < 15.0

        if not self.tx.connected or not self.rx.connected:
            self.link = "OFFLINE"
        elif not tx_fresh or not rx_fresh:
            self.link = "STALE"
        elif self.tx.ack_ok:
            self.link = "ACK OK"
        elif self.tx.local_tx_ok:
            self.link = "ACK TIMEOUT"
        else:
            self.link = "READY"

    def last_rx_age(self) -> float | None:
        if not self.rx.last_packet_mono:
            return None
        return time.monotonic() - self.rx.last_packet_mono
