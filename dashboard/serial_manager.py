"""Serial port discovery, role probing and reconnecting reader threads."""

from __future__ import annotations

import queue
import threading
import time
from collections import deque
from dataclasses import dataclass

import serial
from serial.tools import list_ports

BLUETOOTH_HINTS = ("BTHENUM", "BLUETOOTH", "RFCOMM")


@dataclass
class PortInfo:
    device: str
    description: str = ""
    hwid: str = ""

    @property
    def bluetooth(self) -> bool:
        text = f"{self.description} {self.hwid}".upper()
        return any(h in text for h in BLUETOOTH_HINTS)


def candidate_ports() -> list[PortInfo]:
    ports = [PortInfo(p.device, p.description or "", p.hwid or "")
             for p in list_ports.comports()]
    ports = [p for p in ports if not p.bluetooth]
    ports.sort(key=lambda p: (len(p.device), p.device))
    return ports


def open_serial(port: str, baud: int):
    """Open in the line state that keeps both ESP32 boards running."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.25
    ser.dtr = True
    ser.rts = True
    ser.open()
    try:
        ser.dtr = True
        ser.rts = True
    except Exception:  # noqa: BLE001
        pass
    return ser


def probe_role(port: str, baud: int = 115200, seconds: float = 3.0) -> str | None:
    """Return 'TRANSMITTER' / 'RECEIVER' from firmware evidence, else None."""
    ser = None
    try:
        ser = open_serial(port, baud)
        ser.reset_input_buffer()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace")
            if "ROLE=TRANSMITTER" in line or line.startswith("TXREADY,"):
                return "TRANSMITTER"
            if "ROLE=RECEIVER" in line or line.startswith("RXREADY,"):
                return "RECEIVER"
    except Exception:  # noqa: BLE001
        return None
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
    return None


def detect_ports(preferred_tx: str | None, preferred_rx: str | None,
                 baud: int = 115200, seconds: float = 3.0):
    """Return (tx_port, rx_port, notes). Roles come from the firmware."""
    notes: list[str] = []
    ports = [p.device for p in candidate_ports()]
    if not ports:
        return None, None, ["No serial ports found."]

    tx = None
    rx = None

    if preferred_tx and preferred_tx.upper() in [p.upper() for p in ports]:
        role = probe_role(preferred_tx, baud, seconds)
        if role == "TRANSMITTER":
            tx = preferred_tx
            notes.append(f"{preferred_tx}: TRANSMITTER role verified")
        elif role is None:
            notes.append(f"{preferred_tx}: no role evidence (busy or not flashed)")
        else:
            notes.append(f"{preferred_tx}: reports {role}, not TRANSMITTER")

    if tx is None:
        for port in ports:
            if preferred_tx and port.upper() == preferred_tx.upper():
                continue
            if probe_role(port, baud, seconds) == "TRANSMITTER":
                tx = port
                notes.append(f"{port}: TRANSMITTER discovered")
                break

    if preferred_rx and preferred_rx.lower() not in ("", "auto"):
        if preferred_rx.upper() in [p.upper() for p in ports] and \
                (tx is None or preferred_rx.upper() != tx.upper()):
            role = probe_role(preferred_rx, baud, seconds)
            rx = preferred_rx
            if role == "RECEIVER":
                notes.append(f"{preferred_rx}: RECEIVER role verified")
            else:
                notes.append(f"{preferred_rx}: opened as RECEIVER (role "
                             f"evidence: {role or 'none'})")
        else:
            notes.append(f"{preferred_rx}: not available or already used by TX")

    if rx is None:
        for port in ports:
            if tx and port.upper() == tx.upper():
                continue
            if preferred_rx and port.upper() == preferred_rx.upper():
                continue
            if probe_role(port, baud, seconds) == "RECEIVER":
                rx = port
                notes.append(f"{port}: RECEIVER discovered")
                break

    if tx is None:
        notes.append("TRANSMITTER not detected: is COM4 connected and the "
                     "transmitter firmware flashed?")
    if rx is None:
        notes.append("RECEIVER not detected: connect the second ESP32, close any "
                     "program using its port and flash receiver.ino.")
    return tx, rx, notes


class SerialWorker(threading.Thread):
    """One reconnecting reader thread per port."""

    def __init__(self, port: str, role: str, out: "queue.Queue[tuple]",
                 baud: int = 115200) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.role = role          # "TX" or "RX"
        self.out = out
        self.baud = baud
        self.stop_event = threading.Event()
        self.ser = None
        self._lock = threading.Lock()

    def stop(self) -> None:
        self.stop_event.set()

    def send_line(self, text: str) -> bool:
        with self._lock:
            if self.ser is None or not self.ser.is_open:
                return False
            try:
                self.ser.write((text.rstrip("\r\n") + "\n").encode("utf-8"))
                self.ser.flush()
                return True
            except Exception:  # noqa: BLE001
                return False

    def run(self) -> None:
        while not self.stop_event.is_set():
            ser = None
            try:
                ser = open_serial(self.port, self.baud)
                with self._lock:
                    self.ser = ser
                ser.reset_input_buffer()
                self.out.put(("status", self.role, True,
                              f"{self.role} port {self.port} open @ {self.baud}"))
                while not self.stop_event.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    if line:
                        self.out.put(("line", self.role, line))
            except Exception as exc:  # noqa: BLE001
                if not self.stop_event.is_set():
                    msg = str(exc)
                    if "Access is denied" in msg or "PermissionError" in msg:
                        msg += (" - close the Arduino IDE Serial Monitor or any "
                                "other program using this port")
                    self.out.put(("status", self.role, False,
                                  f"{self.role} port {self.port}: {msg}"))
            finally:
                with self._lock:
                    self.ser = None
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:  # noqa: BLE001
                        pass
            if not self.stop_event.is_set():
                time.sleep(2.0)


class GroundLink:
    """Single shared serial link.

    Multiple browser tabs share one GroundLink (Streamlit cache_resource), so
    the COM ports are opened exactly once. Every session replays the shared
    event log from its own cursor.
    """

    def __init__(self, tx_port: str | None, rx_port: str | None,
                 baud: int = 115200) -> None:
        self.baud = baud
        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.workers: list[SerialWorker] = []
        self.tx_port = tx_port
        self.rx_port = rx_port
        self.tx_worker: SerialWorker | None = None
        self.rx_worker: SerialWorker | None = None
        self.log: "deque[tuple[int, tuple]]" = deque(maxlen=6000)
        self.lock = threading.Lock()
        self.counter = 0
        self.collector_stop = threading.Event()
        self.collector: threading.Thread | None = None

    @property
    def running(self) -> bool:
        return bool(self.workers)

    def start(self) -> None:
        if self.tx_port:
            self.tx_worker = SerialWorker(self.tx_port, "TX", self.queue, self.baud)
            self.tx_worker.start()
            self.workers.append(self.tx_worker)
        if self.rx_port and (not self.tx_port or self.rx_port.upper() != self.tx_port.upper()):
            self.rx_worker = SerialWorker(self.rx_port, "RX", self.queue, self.baud)
            self.rx_worker.start()
            self.workers.append(self.rx_worker)
        self.collector = threading.Thread(target=self._collect, daemon=True)
        self.collector.start()

    def _collect(self) -> None:
        while not self.collector_stop.is_set():
            try:
                event = self.queue.get(timeout=0.2)
            except queue.Empty:
                continue
            with self.lock:
                self.counter += 1
                self.log.append((self.counter, event))

    def send_tx(self, text: str) -> bool:
        return self.tx_worker.send_line(text) if self.tx_worker else False

    def stop(self) -> None:
        self.collector_stop.set()
        for w in self.workers:
            w.stop()
        for w in self.workers:
            w.join(timeout=1.0)
        self.workers.clear()
        if self.collector is not None:
            self.collector.join(timeout=1.0)
            self.collector = None

    def poll(self, state, session: dict) -> None:
        """Apply every event this session has not seen yet."""
        with self.lock:
            pending = [(seq, ev) for seq, ev in self.log
                       if seq > int(session.get("cursor", 0))]
            total = self.counter
        for _, event in pending:
            kind = event[0]
            if kind == "status":
                _, role, connected, message = event
                state.set_connection(role, connected, message)
            elif kind == "line":
                _, role, line = event
                state.feed(role, line)
        session["cursor"] = total
