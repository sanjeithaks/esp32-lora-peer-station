"""Symmetric two-peer LoRa dashboard.

Run:
    python -m streamlit run dashboard/peer_app.py

Both ESP32 boards run the same peer.ino firmware and can send and receive.
No joysticks, no simulated data.
"""

from __future__ import annotations

import json
import queue
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

import streamlit as st

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONFIG_PATH = ROOT / "config" / "settings.json"
sys.path.insert(0, str(HERE))

from serial_manager import SerialWorker, candidate_ports, probe_role  # noqa: E402

st.set_page_config(page_title="LoRa Peer Station", layout="wide",
                   initial_sidebar_state="expanded")

CSS = """
<style>
.stApp { background: radial-gradient(circle at 20% -10%, #0d2036 0%, #050a14 45%); }
[data-testid="stSidebar"] { background:#07101d; border-right:1px solid #16283f; }
html,body,[class*="css"] { color:#dce9f5; font-family:"Segoe UI",system-ui,sans-serif; }
.block-container { padding-top:1.2rem; max-width:1500px; }
.title { font-size:30px; font-weight:700; color:#eef7ff; margin:0; }
.sub { color:#6d8399; font-size:13px; letter-spacing:.08em; text-transform:uppercase; }
.card { background:linear-gradient(180deg,#0c1929,#08111f); border:1px solid #16283f;
  border-radius:12px; padding:14px 16px; }
.panel { border:1px solid #16283f; border-radius:12px; overflow:hidden; margin-bottom:12px; }
.phead { background:#0d1a2d; padding:9px 14px; border-bottom:1px solid #16283f;
  color:#9fb6cc; font-size:12px; font-weight:700; letter-spacing:.12em; text-transform:uppercase; }
.pbody { padding:14px 16px; }
.kv { display:flex; justify-content:space-between; padding:5px 0; border-bottom:1px dashed #122237;
  font-size:13px; }
.kv:last-child { border-bottom:none; }
.k { color:#6d8399; } .v { color:#e6f2ff; font-family:Consolas,monospace; }
.mono { font-family:Consolas,monospace; }
h1,h2,h3,h4,h5 { color:#eef7ff !important; }
[data-testid="stCaptionContainer"], [data-testid="stCaptionContainer"] p { color:#8fa8bf !important; }
[data-testid="stSidebar"] [data-testid="stCaptionContainer"] p { color:#7d95ab !important; }
#MainMenu,footer { visibility:hidden; }
</style>
"""
st.markdown(CSS, unsafe_allow_html=True)


def load_cfg() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_cfg(cfg: dict) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def kv(rows) -> str:
    out = []
    for k, v, c in rows:
        style = f"color:{c}" if c else ""
        out.append(f'<div class="kv"><span class="k">{k}</span>'
                   f'<span class="v" style="{style}">{v}</span></div>')
    return "".join(out)


def panel(title: str, body: str, colour: str = "#38d6ff") -> str:
    return (f'<div class="panel"><div class="phead" style="border-left:3px solid {colour}">'
            f'{title}</div><div class="pbody">{body}</div></div>')


@dataclass
class Peer:
    port: str
    connected: bool = False
    device_id: str = "----"
    radio: bool | None = None
    freq: int | None = None
    stage: str = ""
    last_seq: int | None = None
    last_type: str | None = None
    last_text: str = ""
    last_values: list[int | None] = field(default_factory=lambda: [None] * 4)
    last_ack: bool | None = None
    last_result: str = ""
    rssi: int | None = None
    snr: float | None = None
    rx_count: int = 0
    rx_from: str = ""
    rx_time: str = ""
    tx_count: int = 0
    ack_count: int = 0
    stage_mono: float = 0.0


class PeerLink:
    def __init__(self, ports: list[str], baud: int = 115200) -> None:
        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.workers: list[SerialWorker] = []
        for i, port in enumerate(ports):
            w = SerialWorker(port, f"P{i}", self.queue, baud)
            w.start()
            self.workers.append(w)

    def send(self, index: int, text: str) -> bool:
        if index >= len(self.workers):
            return False
        return self.workers[index].send_line(text)

    def stop(self) -> None:
        for w in self.workers:
            w.stop()
        for w in self.workers:
            w.join(timeout=0.5)


def detect_peers(max_ports: int = 2) -> tuple[list[str], list[str]]:
    ports = [p.device for p in candidate_ports()]
    peers: list[str] = []
    notes: list[str] = []
    for port in ports:
        role = probe_role(port, 115200, 2.5)
        if role == "PEER" or role is None:
            # Peer firmware identifies itself in READY; probe_role returns None
            # for it, so open a short read to confirm.
            pass
    # Direct probe for PEERREADY
    import serial
    for port in ports:
        if len(peers) >= max_ports:
            break
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 0.25
            s.dtr = True
            s.rts = True
            s.open()
            s.dtr = True
            s.rts = True
            s.reset_input_buffer()
            deadline = time.monotonic() + 2.5
            found = False
            while time.monotonic() < deadline and not found:
                line = s.readline().decode("utf-8", "replace")
                if line.startswith("PEERREADY") or "LORA PEER" in line:
                    found = True
            s.close()
            if found:
                peers.append(port)
                notes.append(f"{port}: peer detected")
        except Exception as exc:  # noqa: BLE001
            notes.append(f"{port}: {exc}")
    return peers, notes


def parse_line(peer: Peer, line: str) -> None:
    if line.startswith("PEERREADY,"):
        for part in line.split(",")[1:]:
            k, _, v = part.partition("=")
            if k == "id":
                peer.device_id = v
            elif k == "radio":
                peer.radio = v == "OK"
            elif k == "freq":
                try:
                    peer.freq = int(v)
                except ValueError:
                    peer.freq = None
        peer.connected = True
        return
    if line.startswith("TXBEGIN,"):
        kvp = dict(p.split("=", 1) for p in line.split(",")[1:] if "=" in p)
        peer.last_seq = int(kvp.get("seq", "0"))
        peer.last_type = kvp.get("type")
        peer.stage = "TEXT READY" if peer.last_type == "T" else "VALUES READY"
        peer.stage_mono = time.monotonic()
        return
    if line.startswith("TXSTAGE,"):
        kvp = dict(p.split("=", 1) for p in line.split(",")[1:] if "=" in p)
        peer.stage = kvp.get("stage", "")
        peer.stage_mono = time.monotonic()
        if peer.stage == "ACK_RECEIVED":
            peer.last_ack = True
        elif peer.stage in ("DELIVERY_CONFIRMED",):
            peer.last_ack = True
            peer.last_result = "DELIVERY CONFIRMED"
        elif peer.stage in ("DELIVERY_NOT_CONFIRMED", "TRANSMISSION_FAILED"):
            peer.last_ack = False
            peer.last_result = peer.stage.replace("_", " ")
        return
    if line.startswith("PKTSTAT,"):
        kvp = dict(p.split("=", 1) for p in line.split(",")[1:] if "=" in p)
        peer.rssi = int(kvp.get("rssi", "0") or 0) or None
        peer.snr = float(kvp.get("snr", "0") or 0.0)
        peer.tx_count = int(kvp.get("txok", "0") or 0)
        peer.ack_count = int(kvp.get("ackok", "0") or 0)
        peer.last_ack = kvp.get("ack") == "OK"
        peer.last_result = ("DELIVERY CONFIRMED" if peer.last_ack
                            else "DELIVERY NOT CONFIRMED")
        return
    if line.startswith("RXPKT,"):
        kvp = dict(p.split("=", 1) for p in line.split(",")[1:] if "=" in p)
        if kvp.get("dup") == "1":
            return
        peer.rx_count += 1
        peer.rx_from = kvp.get("from", "")
        if peer.stage.startswith("RECEIVER ERROR"):
            # A valid packet supersedes a transient reception error.
            peer.stage = ""
        peer.last_seq = int(kvp.get("seq", "0"))
        peer.last_type = kvp.get("type")
        peer.rssi = int(kvp.get("rssi", "0"))
        peer.snr = float(kvp.get("snr", "0"))
        peer.rx_time = time.strftime("%H:%M:%S")
        if peer.last_type == "T":
            try:
                peer.last_text = bytes.fromhex(kvp.get("text", "")).decode("utf-8", "replace")
            except ValueError:
                peer.last_text = ""
            peer.last_values = [None] * 4
        else:
            peer.last_values = [int(kvp.get(f"c{i}", "0")) for i in range(1, 5)]
            peer.last_text = ""
        return
    if line.startswith("RXERR,"):
        peer.stage = "RECEIVER ERROR: " + line.split(",", 1)[1]
        peer.stage_mono = time.monotonic()


@st.cache_resource(show_spinner=False)
def get_link(ports: tuple[str, ...]) -> PeerLink:
    link = PeerLink(list(ports))
    return link


cfg = load_cfg()
if "peers" not in st.session_state:
    detected, notes = detect_peers()
    st.session_state.notes = notes
    st.session_state.peers = [Peer(p) for p in detected]
    st.session_state.ports = tuple(detected)
if "link" not in st.session_state:
    st.session_state.link = get_link(tuple(st.session_state.ports))

link: PeerLink = st.session_state.link
peers: list[Peer] = st.session_state.peers


def poll() -> None:
    for _ in range(800):
        try:
            event = link.queue.get_nowait()
        except queue.Empty:
            return
        kind = event[0]
        if kind == "status":
            _, role, connected, msg = event
            try:
                idx = int(role[1:])
            except ValueError:
                continue
            if idx < len(peers):
                peers[idx].connected = connected
                if not connected:
                    peers[idx].radio = None
        elif kind == "line":
            _, role, line = event
            try:
                idx = int(role[1:])
            except ValueError:
                continue
            if idx < len(peers):
                parse_line(peers[idx], line)


poll()

with st.sidebar:
    st.markdown("### PEER STATION")
    page = st.radio("PAGE", ["Peers", "Diagnostics"], label_visibility="collapsed")
    st.markdown("---")
    st.markdown("##### PORTS")
    p1 = st.text_input("Peer 1 port", value=st.session_state.ports[0] if len(st.session_state.ports) > 0 else "COM4")
    p2 = st.text_input("Peer 2 port", value=st.session_state.ports[1] if len(st.session_state.ports) > 1 else "COM6")
    if st.button("Apply & Reconnect", use_container_width=True):
        link.stop()
        get_link.clear()
        ports = tuple(p.strip().upper() for p in (p1, p2) if p.strip())
        st.session_state.ports = ports
        st.session_state.peers = [Peer(p) for p in ports]
        st.session_state.link = get_link(ports)
        st.rerun()
    st.markdown("---")
    for n in st.session_state.notes:
        st.caption(n)
    st.caption("Close the Arduino Serial Monitors: this dashboard owns the ports.")

st.markdown('<div class="title">LORA BIDIRECTIONAL PEER STATION</div>', unsafe_allow_html=True)
st.markdown('<div class="sub">ESP32 #1 and ESP32 #2 can each transmit and receive</div>',
            unsafe_allow_html=True)
st.markdown("<div style='height:12px'></div>", unsafe_allow_html=True)

if page == "Peers":
    cols = st.columns(2)
    for i, col in enumerate(cols):
        with col:
            if i >= len(peers):
                st.info("Peer not connected. Set its COM port in the sidebar.")
                continue
            peer = peers[i]
            colour = "#2fe0a0" if peer.connected else "#ff4f60"
            st.markdown(panel(
                f"ESP32 #{i + 1} - {peer.port}",
                kv([
                    ("Connection", "CONNECTED" if peer.connected else "OFFLINE", colour),
                    ("Device ID", peer.device_id, None),
                    ("LoRa", "INITIALIZED" if peer.radio else
                     "FAILED" if peer.radio is False else "-",
                     "#2fe0a0" if peer.radio else "#ff4f60"),
                    ("Frequency", f"{peer.freq} Hz" if peer.freq else "INVALID",
                     "#2fe0a0" if peer.freq and 432000000 < peer.freq < 434000000 else "#ff4f60"),
                    ("TX status", peer.stage or "READY",
                     "#2fe0a0" if "CONFIRMED" in peer.stage and "NOT" not in peer.stage
                     else "#ffb224"),
                    ("ACK", "RECEIVED" if peer.last_ack else
                     "TIMEOUT" if peer.last_ack is False else "-",
                     "#2fe0a0" if peer.last_ack else "#ff4f60" if peer.last_ack is False else None),
                    ("Last result", peer.last_result or "-", None),
                    ("TX ok / ACK", f"{peer.tx_count} / {peer.ack_count}", None),
                    ("RX packets", str(peer.rx_count), None),
                    ("RSSI / SNR", f"{peer.rssi} dBm / {peer.snr:.2f} dB"
                     if peer.rssi is not None else "-", None),
                ]), colour), unsafe_allow_html=True)

            text = st.text_input(f"Text from ESP32 #{i + 1}", key=f"text{i}",
                                 placeholder="HELLO FROM ESP32")
            if st.button(f"TRANSMIT TEXT #{i + 1}", key=f"btext{i}",
                         use_container_width=True):
                if text.strip() and link.send(i, "TEXT," + text.strip()):
                    peer.stage = "TEXT SENT TO RADIO"
                    st.rerun()
                elif text.strip():
                    st.error("Port is not open.")
                else:
                    st.warning("Enter a message.")

            nums = st.text_input(f"Values from ESP32 #{i + 1}", key=f"num{i}",
                                 placeholder="100,200,300,400")
            if st.button(f"TRANSMIT VALUES #{i + 1}", key=f"bnum{i}",
                         use_container_width=True):
                parts = [x.strip() for x in nums.split(",")]
                if len(parts) == 4 and all(p.isdigit() and int(p) <= 4095 for p in parts):
                    if link.send(i, "DATA," + ",".join(parts)):
                        peer.stage = "VALUES SENT TO RADIO"
                        st.rerun()
                    else:
                        st.error("Port is not open.")
                else:
                    st.warning("Enter four numbers 0..4095, comma separated.")

            st.markdown(
                f'<div class="card" style="margin-top:8px"><div class="k">RECEIVED FROM PEER</div>'
                f'<div class="v" style="font-size:16px">'
                f'{peer.last_text or (", ".join(str(v) if v is not None else "-" for v in peer.last_values) if any(v is not None for v in peer.last_values) else "-")}'
                f'</div><div class="k" style="margin-top:6px">from {peer.rx_from or "-"} '
                f'at {peer.rx_time or "-"}</div></div>', unsafe_allow_html=True)
else:
    st.markdown("### DIAGNOSTICS")
    for i, peer in enumerate(peers):
        st.markdown(f"#### ESP32 #{i + 1} ({peer.port})")
        st.markdown(kv([
            ("Serial", "CONNECTED" if peer.connected else "OFFLINE",
             "#2fe0a0" if peer.connected else "#ff4f60"),
            ("Device ID", peer.device_id, None),
            ("Radio", "INITIALIZED" if peer.radio else "FAILED",
             "#2fe0a0" if peer.radio else "#ff4f60"),
            ("Frequency", f"{peer.freq} Hz" if peer.freq else "INVALID - check power/SPI",
             "#2fe0a0" if peer.freq and 432000000 < peer.freq < 434000000 else "#ff4f60"),
            ("Last stage", peer.stage or "READY", None),
            ("Last ACK", "RECEIVED" if peer.last_ack else
             "TIMEOUT" if peer.last_ack is False else "-", None),
        ]), unsafe_allow_html=True)
    st.caption("A frequency of 0 Hz or version 0x00 is an SPI/power fault at that "
               "module, even if an earlier init succeeded.")

if cfg.get("auto_refresh", True):
    time.sleep(0.6)
    st.rerun()
