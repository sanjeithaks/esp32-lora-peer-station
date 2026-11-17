"""System diagnostics: identify which side is failing and what to check."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from telemetry_parser import GroundState

OK = "OK"
FAIL = "ERROR"
WARN = "WARNING"
WAIT = "WAITING"
INFO = "INFO"


@dataclass
class Check:
    label: str
    state: str
    detail: str
    recommendation: str = ""

    @property
    def colour(self) -> str:
        return {
            OK: "#2fe0a0",
            FAIL: "#ff4f60",
            WARN: "#ffb224",
            WAIT: "#6d8399",
            INFO: "#38d6ff",
        }.get(self.state, "#6d8399")


def _missing(value) -> bool:
    return value is None


def evaluate(state: GroundState, link_timeout_s: float = 8.0) -> list[Check]:
    checks: list[Check] = []
    tx, rx = state.tx, state.rx

    # ---------------- transmitter
    checks.append(Check(
        "Transmitter USB / serial",
        OK if tx.connected else FAIL,
        f"{tx.port or 'not selected'} " + ("connected" if tx.connected else "unavailable"),
        "" if tx.connected else
        "Connect the transmitter ESP32 and close any program using its serial port.",
    ))
    checks.append(Check(
        "Transmitter firmware role",
        OK if tx.role_ok else (WAIT if tx.connected else FAIL),
        "TRANSMITTER role verified" if tx.role_ok else "no role evidence yet",
        "" if tx.role_ok else "Flash transmitter.ino on the transmitter board.",
    ))
    checks.append(Check(
        "Transmitter LoRa / SPI",
        OK if tx.radio_ok else (FAIL if tx.radio_ok is False else WAIT),
        "SX1278 initialised at 433 MHz" if tx.radio_ok else "no radio status",
        "" if tx.radio_ok else
        "Check 3.3 V, GND, SCK=18, MISO=19, MOSI=23, NSS=5, RST=14, DIO0=26.",
    ))

    # ---------------- receiver
    checks.append(Check(
        "Receiver USB / serial",
        OK if rx.connected else FAIL,
        f"{rx.port or 'auto-detect failed'} " + ("connected" if rx.connected else "unavailable"),
        "" if rx.connected else
        "Connect the second ESP32, close the Arduino Serial Monitor using that port, "
        "and set the receiver port in the sidebar if auto-detection fails.",
    ))
    checks.append(Check(
        "Receiver firmware role",
        OK if rx.role_ok else (WAIT if rx.connected else FAIL),
        "RECEIVER role verified" if rx.role_ok else "no role evidence yet",
        "" if rx.role_ok else "Flash receiver.ino on the second board.",
    ))
    checks.append(Check(
        "Receiver LoRa / SPI",
        OK if rx.radio_ok else (FAIL if rx.radio_ok is False else WAIT),
        "SX1278 initialised at 433 MHz" if rx.radio_ok else "no radio status",
        "" if rx.radio_ok else
        "Check receiver power/SPI wiring and that an antenna is fitted.",
    ))

    # ---------------- link
    age = state.last_rx_age()
    fresh = age is not None and age <= link_timeout_s
    checks.append(Check(
        "Packet reception",
        OK if rx.valid > 0 else WAIT,
        f"{rx.valid} valid packet(s) received" if rx.valid else "no packet received yet",
        "" if rx.valid else "Enter SEND,... or a message on the transmitter console.",
    ))
    checks.append(Check(
        "Link freshness",
        OK if fresh else (WARN if rx.valid else WAIT),
        f"last packet {age:.1f} s ago" if age is not None else "no packet received yet",
        "" if fresh else
        "No recent LoRa packet. Check both antennas, frequency, sync word and power.",
    ))
    checks.append(Check(
        "Acknowledgement",
        OK if tx.ack_ok else (FAIL if tx.local_tx_ok else WAIT),
        ("ACK received from receiver" if tx.ack_ok else
         "ACK timeout - receiver did not confirm" if tx.local_tx_ok else
         "no transmission yet"),
        "" if tx.ack_ok else
        "Check the receiver is powered and receiving, antennas are fitted and both "
        "boards use 433 MHz / sync word 0x12.",
    ))

    # ---------------- data quality
    checks.append(Check(
        "Invalid packets",
        OK if rx.invalid == 0 else WARN,
        f"{rx.invalid} invalid packet(s)",
        "" if rx.invalid == 0 else
        "The receiver rejected malformed packets. Check the transmitter payload format.",
    ))
    checks.append(Check(
        "Duplicate packets",
        INFO,
        f"{rx.duplicates} duplicate packet(s)",
    ))
    checks.append(Check(
        "Packet loss (from sequence gaps)",
        OK,
        f"{rx.lost}",
    ))
    return checks


def overall(state: GroundState, checks: Iterable[Check]) -> tuple[str, str]:
    """Return (overall status, colour)."""
    checks = list(checks)
    failing = [c for c in checks if c.state == FAIL]
    waiting = [c for c in checks if c.state == WAIT]
    tx, rx = state.tx, state.rx

    if failing:
        return "SYSTEM ERROR", "#ff4f60"
    if tx.ack_ok and rx.valid > 0 and tx.radio_ok and rx.radio_ok:
        return "OPERATIONAL", "#2fe0a0"
    if tx.connected and rx.connected and (waiting or rx.valid == 0):
        return "STANDBY / WAITING", "#ffb224"
    return "NOT OPERATIONAL", "#ff4f60"


def recommendations(checks: Iterable[Check]) -> list[str]:
    seen: list[str] = []
    for c in checks:
        if c.recommendation and c.state in (FAIL, WARN) and c.recommendation not in seen:
            seen.append(c.recommendation)
    return seen
