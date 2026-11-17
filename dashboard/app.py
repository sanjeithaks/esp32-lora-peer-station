"""LoRa Ground Station - Streamlit dashboard.

Run from the project root:
    python -m streamlit run dashboard/app.py

All values come from the two ESP32 boards over USB serial. Nothing is simulated.
Manual input is typed on the transmitter (Arduino Serial Monitor COM4) or in the
optional sidebar field; the dashboard itself only displays real received data.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import streamlit as st

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONFIG_PATH = ROOT / "config" / "settings.json"

sys.path.insert(0, str(HERE))

from telemetry_parser import GroundState  # noqa: E402
from serial_manager import GroundLink, detect_ports, candidate_ports  # noqa: E402
import diagnostics  # noqa: E402

st.set_page_config(
    page_title="LoRa Ground Station",
    layout="wide",
    initial_sidebar_state="expanded",
)

CSS = """
<style>
:root {
  --bg:#050a14; --panel:#0a1424; --panel2:#0d1a2d; --edge:#16283f;
  --text:#dce9f5; --muted:#6d8399; --cyan:#38d6ff; --green:#2fe0a0;
  --amber:#ffb224; --red:#ff4f60;
}
.stApp { background: radial-gradient(circle at 20% -10%, #0d2036 0%, #050a14 45%); }
[data-testid="stSidebar"] { background:#07101d; border-right:1px solid #16283f; }
html, body, [class*="css"] { color: var(--text); font-family:"Segoe UI", system-ui, sans-serif; }
.block-container { padding-top:1.2rem; padding-bottom:2rem; max-width:1500px; }
.gs-title { font-size:30px; font-weight:700; color:#eef7ff; margin:0; }
.gs-sub { color:var(--muted); font-size:13px; letter-spacing:.08em; text-transform:uppercase; }
.gs-badge { display:inline-block; padding:6px 14px; border-radius:20px; font-weight:700;
  font-size:12px; letter-spacing:.12em; }
.card { background:linear-gradient(180deg,#0c1929 0%,#08111f 100%);
  border:1px solid var(--edge); border-radius:12px; padding:14px 16px; }
.card h4 { margin:0 0 2px 0; color:var(--muted); font-size:11px; letter-spacing:.14em;
  text-transform:uppercase; font-weight:600; }
.card .big { font-size:22px; font-weight:700; color:#e8f4ff; }
.card .small { color:var(--muted); font-size:12px; }
.panel { background:var(--panel); border:1px solid var(--edge); border-radius:12px;
  overflow:hidden; margin-bottom:14px; }
.panel-head { background:var(--panel2); padding:9px 14px; border-bottom:1px solid var(--edge);
  color:#9fb6cc; font-size:12px; font-weight:700; letter-spacing:.12em; text-transform:uppercase; }
.panel-body { padding:14px 16px; }
.kv { display:flex; justify-content:space-between; padding:5px 0;
  border-bottom:1px dashed #122237; font-size:13px; }
.kv:last-child { border-bottom:none; }
.kv .k { color:var(--muted); }
.kv .v { color:#e6f2ff; font-family:Consolas, monospace; text-align:right; }
.channel { background:linear-gradient(180deg,#0d2033,#081422);
  border:1px solid #1b3550; border-radius:12px; padding:12px; text-align:center; }
.channel .lbl { color:var(--muted); font-size:11px; letter-spacing:.14em; }
.channel .val { font-family:Consolas, monospace; font-size:34px; font-weight:700;
  color:var(--cyan); line-height:1.2; }
.channel .sub { color:#4d6a86; font-size:11px; }
.mono { font-family:Consolas, monospace; }
#MainMenu, footer { visibility:hidden; }
</style>
"""
st.markdown(CSS, unsafe_allow_html=True)


def load_config() -> dict:
    try:
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {
            "transmitter_port": "COM4",
            "receiver_port": "auto",
            "baudrate": 115200,
            "refresh_seconds": 0.6,
            "probe_seconds": 3.0,
            "history_limit": 200,
            "link_timeout_ms": 8000,
            "auto_refresh": True,
        }


def save_config(cfg: dict) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def badge(text: str, colour: str) -> str:
    return (
        f'<span class="gs-badge" style="background:{colour}22;'
        f'border:1px solid {colour};color:{colour}">{text}</span>'
    )


def stage_colour(stage: str) -> str:
    text = (stage or "").upper()
    if "NOT CONFIRMED" in text or "TIMEOUT" in text or "FAILED" in text \
            or "NOT OPEN" in text:
        return "#ff4f60"
    if "CONFIRMED" in text or "ACK RECEIVED" in text:
        return "#2fe0a0"
    if any(w in text for w in ("SENT", "STARTED", "TX COMPLETE", "WAITING")):
        return "#ffb224"
    return "#6d8399"


def kv_rows(rows: list[tuple[str, str, str | None]]) -> str:
    out = []
    for key, value, colour in rows:
        style = f' style="color:{colour}"' if colour else ""
        out.append(
            f'<div class="kv"><span class="k">{key}</span>'
            f'<span class="v"{style}>{value}</span></div>'
        )
    return "".join(out)


def panel(title: str, body_html: str, accent: str = "#38d6ff") -> str:
    return (
        f'<div class="panel"><div class="panel-head" '
        f'style="border-left:3px solid {accent}">{title}</div>'
        f'<div class="panel-body">{body_html}</div></div>'
    )


cfg = load_config()
if "state" not in st.session_state:
    st.session_state.state = GroundState(history_limit=cfg.get("history_limit", 200))


@st.cache_resource(show_spinner=False)
def get_link():
    """One serial link for the whole server (single owner of the COM ports)."""
    saved = load_config()
    tx0 = saved.get("transmitter_port")
    if tx0 in (None, "", "auto"):
        tx0 = None
    rx0 = saved.get("receiver_port")
    if rx0 in (None, "", "auto"):
        rx0 = None
    tx, rx, port_notes = detect_ports(
        tx0, rx0, saved.get("baudrate", 115200), saved.get("probe_seconds", 3.0)
    )
    ground_link = GroundLink(tx, rx, saved.get("baudrate", 115200))
    ground_link.start()
    return ground_link, tx, rx, port_notes


state: GroundState = st.session_state.state

with st.spinner("Verifying serial ports..."):
    link, detected_tx, detected_rx, notes = get_link()

if "cursor" not in st.session_state:
    st.session_state.cursor = 0
link.poll(state, st.session_state)
state.update_link()
if state.tx.port is None:
    state.tx.port = detected_tx
if state.rx.port is None:
    state.rx.port = detected_rx

# Optional deep link: http://localhost:8501/?cmd=SEND,1500,1000,2000,1500
auto_cmd = st.query_params.get("cmd")
if auto_cmd and not st.session_state.get("_auto_cmd_sent"):
    if link.send_tx(auto_cmd):
        st.session_state["_auto_cmd_sent"] = True

# --------------------------------------------------------------------------- sidebar
with st.sidebar:
    st.markdown("### GROUND STATION")
    page = st.radio("PAGE", ["Ground Station", "System Diagnostics"],
                    label_visibility="collapsed")
    st.markdown("---")
    st.markdown("##### PORTS")

    tx_port = st.text_input("Transmitter (auto or COMx)",
                            value=cfg.get("transmitter_port", "auto"))
    rx_value = cfg.get("receiver_port", "auto")
    rx_port = st.text_input("Receiver (auto or COMx)",
                            value="auto" if rx_value in (None, "", "auto") else str(rx_value))

    if st.button("Apply & Reconnect", use_container_width=True):
        link.stop()
        new_cfg = dict(cfg)
        new_cfg["transmitter_port"] = (
            tx_port.strip().upper() if tx_port.strip().lower() != "auto" else "auto"
        )
        new_cfg["receiver_port"] = (
            rx_port.strip().upper() if rx_port.strip().lower() != "auto" else "auto"
        )
        save_config(new_cfg)
        st.session_state.state = GroundState(history_limit=new_cfg.get("history_limit", 200))
        get_link.clear()
        st.session_state.cursor = 0
        st.session_state.pop("_auto_cmd_sent", None)
        st.rerun()

    st.markdown("---")
    st.markdown("##### TRANSMIT (optional)")
    st.caption("Same input as the transmitter Serial Monitor. The Serial Monitor "
               "cannot be open at the same time as this dashboard.")
    manual = st.text_input("Command", key="manual_command",
                           placeholder="SEND,1500,1000,2000,1500")
    if st.button("Transmit", use_container_width=True):
        if not manual.strip():
            st.warning("Enter SEND,<c1>,<c2>,<c3>,<c4> or a message.")
        elif link.send_tx(manual.strip()):
            st.success("Sent to transmitter.")
        else:
            st.error("Transmitter port is not open.")

    st.markdown("---")
    st.markdown("##### DETECTED")
    st.markdown(f"**TX:** `{state.tx.port or 'none'}`  \n"
                f"**RX:** `{state.rx.port or 'none'}`")
    for note in notes:
        st.caption(note)
    st.markdown("---")
    st.caption("Only one program can use a COM port. Close the Arduino Serial "
               "Monitors while this dashboard is running.")


def render_status_cards() -> None:
    tx, rx = state.tx, state.rx
    tx_state = ("CONNECTED" if tx.connected and tx.role_ok else
                "PORT OPEN" if tx.connected else "OFFLINE")
    tx_colour = ("#2fe0a0" if tx.connected and tx.role_ok else
                 "#ffb224" if tx.connected else "#ff4f60")
    rx_state = ("CONNECTED" if rx.connected and rx.role_ok else
                "PORT OPEN" if rx.connected else "OFFLINE")
    rx_colour = ("#2fe0a0" if rx.connected and rx.role_ok else
                 "#ffb224" if rx.connected else "#ff4f60")

    if state.link == "ACK OK":
        link_state, link_colour = "ACK RECEIVED", "#2fe0a0"
    elif state.link == "ACK TIMEOUT":
        link_state, link_colour = "ACK TIMEOUT", "#ffb224"
    elif state.link in ("STALE", "OFFLINE"):
        link_state, link_colour = state.link, "#ffb224" if state.link == "STALE" else "#ff4f60"
    else:
        link_state, link_colour = state.link, "#6d8399"

    c1, c2, c3 = st.columns(3)
    with c1:
        st.markdown(
            f'<div class="card"><h4>TRANSMITTER STATUS</h4>'
            f'<div class="big" style="color:{tx_colour}">{tx_state}</div>'
            f'<div class="small">{tx.port or "no port"} | SX1278 '
            f'{"OK" if tx.radio_ok else "FAIL" if tx.radio_ok is False else "?"}'
            f'</div></div>', unsafe_allow_html=True)
    with c2:
        st.markdown(
            f'<div class="card"><h4>RECEIVER STATUS</h4>'
            f'<div class="big" style="color:{rx_colour}">{rx_state}</div>'
            f'<div class="small">{rx.port or "auto-detect failed"} | SX1278 '
            f'{"OK" if rx.radio_ok else "FAIL" if rx.radio_ok is False else "?"}'
            f'</div></div>', unsafe_allow_html=True)
    with c3:
        st.markdown(
            f'<div class="card"><h4>WIRELESS COMMUNICATION</h4>'
            f'<div class="big" style="color:{link_colour}">{link_state}</div>'
            f'<div class="small">RSSI {rx.rssi if rx.rssi is not None else "-"} dBm | '
            f'SNR {f"{rx.snr:.2f}" if rx.snr is not None else "-"} dB</div></div>',
            unsafe_allow_html=True)


# --------------------------------------------------------------------------- page 1
if page == "Ground Station":
    checks = diagnostics.evaluate(state, cfg.get("link_timeout_ms", 8000) / 1000)
    overall, overall_colour = diagnostics.overall(state, checks)

    head_l, head_r = st.columns([3, 2])
    with head_l:
        st.markdown('<div class="gs-title">LORA WIRELESS COMMUNICATION SYSTEM</div>',
                    unsafe_allow_html=True)
        st.markdown('<div class="gs-sub">ESP32 manual telemetry transmission and '
                    'ground station monitoring</div>', unsafe_allow_html=True)
    with head_r:
        st.markdown(
            f'<div style="text-align:right;padding-top:6px">'
            f'{badge(overall, overall_colour)}'
            f'<div class="small mono" style="margin-top:8px">'
            f'{time.strftime("%Y-%m-%d %H:%M:%S")}</div></div>',
            unsafe_allow_html=True)

    st.markdown("<div style='height:10px'></div>", unsafe_allow_html=True)
    render_status_cards()
    st.markdown("<div style='height:14px'></div>", unsafe_allow_html=True)

    with st.container(border=True):
        st.markdown("##### MANUAL TRANSMITTER INPUT")
        st.caption("Type a packet and press TRANSMIT. This is written to the "
                   "transmitter board exactly like typing in its Serial Monitor.")
        send_col, btn_col = st.columns([5, 1])
        with send_col:
            dash_cmd = st.text_input(
                "Packet",
                key="main_command",
                label_visibility="collapsed",
                placeholder="SEND,1500,1000,2000,1500   or   HELLO RECEIVER",
            )
        with btn_col:
            send_now = st.button("TRANSMIT", key="main_send",
                                 use_container_width=True)
        if send_now:
            entered = dash_cmd.strip()
            if not entered:
                st.warning("Enter SEND,<c1>,<c2>,<c3>,<c4> or a message.")
            else:
                parts = entered.split(",")
                parsed = None
                if parts[0].strip().upper() == "SEND" and len(parts) == 5:
                    try:
                        parsed = [int(p.strip()) for p in parts[1:]]
                    except ValueError:
                        parsed = None
                state.tx.outgoing = entered
                state.tx.stage = "COMMAND SENT TO " + (link.tx_port or "TRANSMITTER")
                state.tx.stage_seq = None
                if parsed is not None:
                    state.tx.channels = parsed
                state.add_event(f"Manual payload entered: {entered}", "info")
                if link.send_tx(entered):
                    state.add_event(
                        f"Command written to transmitter port {link.tx_port}", "info")
                    st.success("Command sent to the transmitter. Waiting for its "
                               "status lines.")
                else:
                    state.tx.stage = "TRANSMITTER PORT NOT OPEN"
                    state.add_event("Transmitter port is not open", "err")
                    st.error("Transmitter port is not open. Close the Arduino "
                             "Serial Monitor or check the ports in the sidebar.")

        stage = state.tx.stage or "READY"
        ch = state.tx.channels
        ch_text = ", ".join(str(c) if c is not None else "-" for c in ch) \
            if ch else "-"
        seq = state.tx.stage_seq if state.tx.stage_seq is not None else state.tx.sequence
        st.markdown(
            panel("OUTGOING DATA", kv_rows([
                ("Entered payload", state.tx.outgoing or "-", None),
                ("Packet sequence", str(seq) if seq is not None else "-", None),
                ("Channel 1..4", ch_text, None),
                ("Transmission status", stage, stage_colour(stage)),
                ("Last result", state.tx.last_result or "-",
                 "#2fe0a0" if state.tx.ack_ok else
                 "#ff4f60" if state.tx.last_result else None),
            ]), "#38d6ff"), unsafe_allow_html=True)

    st.markdown("<div style='height:14px'></div>", unsafe_allow_html=True)

    rx = state.rx
    st.markdown('<div class="panel-head" style="border-left:3px solid #38d6ff;'
                'border-radius:10px">LIVE TELEMETRY (from LoRa receiver)</div>',
                unsafe_allow_html=True)
    cols = st.columns(4)
    for i, col in enumerate(cols):
        value = rx.channels[i] if rx.channels and rx.channels[i] is not None else "-"
        col.markdown(
            f'<div class="channel"><div class="lbl">CHANNEL {i + 1}</div>'
            f'<div class="val">{value}</div>'
            f'<div class="sub">packet {rx.sequence if rx.sequence is not None else "-"}'
            f'</div></div>', unsafe_allow_html=True)

    if rx.text:
        st.markdown(
            f'<div class="card" style="margin-top:12px"><h4>RECEIVED MESSAGE</h4>'
            f'<div class="big" style="font-size:18px">{rx.text}</div></div>',
            unsafe_allow_html=True)

    st.markdown("<div style='height:14px'></div>", unsafe_allow_html=True)
    left, right = st.columns(2)
    tx = state.tx

    tx_rows = [
        ("COM port", tx.port or "-", None),
        ("ESP32 / firmware", "CONNECTED" if tx.role_ok else "NO ROLE EVIDENCE",
         "#2fe0a0" if tx.role_ok else "#ffb224"),
        ("LoRa initialisation", "INITIALIZED" if tx.radio_ok else
         "FAILED" if tx.radio_ok is False else "WAITING",
         "#2fe0a0" if tx.radio_ok else "#ff4f60" if tx.radio_ok is False else None),
        ("Transmissions attempted", str(tx.tx_ok_count + tx.tx_fail_count), None),
        ("Local TX success", str(tx.tx_ok_count), "#2fe0a0"),
        ("Local TX failure", str(tx.tx_fail_count), "#ff4f60" if tx.tx_fail_count else None),
        ("ACK received", str(tx.ack_ok_count), "#2fe0a0"),
        ("ACK timeouts", str(tx.ack_timeout_count),
         "#ffb224" if tx.ack_timeout_count else None),
        ("Last packet", f"{tx.sequence} ({tx.packet_type or '-'})"
         if tx.sequence is not None else "-", None),
        ("Last transmission", tx.last_wall or "-", None),
        ("Last result", tx.last_result or "-",
         "#2fe0a0" if tx.ack_ok else "#ffb224" if tx.local_tx_ok else None),
        ("Last input", ", ".join(str(c) for c in tx.channels)
         if tx.channels and any(c is not None for c in tx.channels) else "-", None),
    ]
    rx_rows = [
        ("COM port", rx.port or "-", None),
        ("ESP32 / firmware", "CONNECTED" if rx.role_ok else "NO ROLE EVIDENCE",
         "#2fe0a0" if rx.role_ok else "#ffb224"),
        ("LoRa initialisation", "INITIALIZED" if rx.radio_ok else
         "FAILED" if rx.radio_ok is False else "WAITING",
         "#2fe0a0" if rx.radio_ok else "#ff4f60" if rx.radio_ok is False else None),
        ("Packets received", str(rx.received), None),
        ("Valid / invalid", f"{rx.valid} / {rx.invalid}",
         "#ff4f60" if rx.invalid else None),
        ("Duplicates", str(rx.duplicates), "#ffb224" if rx.duplicates else None),
        ("Packet loss", str(rx.lost), "#ffb224" if rx.lost else None),
        ("RSSI", f"{rx.rssi} dBm" if rx.rssi is not None else "-", None),
        ("SNR", f"{rx.snr:.2f} dB" if rx.snr is not None else "-", None),
        ("ACK sent", "OK" if rx.ack_ok else "FAILED" if rx.ack_ok is False else "-",
         "#2fe0a0" if rx.ack_ok else "#ff4f60" if rx.ack_ok is False else None),
        ("Last packet", f"{rx.sequence} ({rx.packet_type or '-'})"
         if rx.sequence is not None else "-", None),
        ("Last reception", rx.last_wall or "-", None),
    ]

    with left:
        st.markdown(panel("TRANSMITTER PANEL", kv_rows(tx_rows), "#38d6ff"),
                    unsafe_allow_html=True)
        st.markdown(panel("RECEIVER / GROUND STATION PANEL", kv_rows(rx_rows), "#2fe0a0"),
                    unsafe_allow_html=True)

    with right:
        st.markdown(panel("PACKET STATISTICS", kv_rows([
            ("Received", str(rx.received), None),
            ("Valid", str(rx.valid), "#2fe0a0"),
            ("Invalid", str(rx.invalid), "#ff4f60" if rx.invalid else None),
            ("Duplicate", str(rx.duplicates), "#ffb224" if rx.duplicates else None),
            ("Loss (sequence gaps)", str(rx.lost), "#ffb224" if rx.lost else None),
            ("ACK success", str(rx.ack_sent), None),
            ("ACK failures", str(rx.ack_failed), "#ff4f60" if rx.ack_failed else None),
        ]), "#ffb224"), unsafe_allow_html=True)

        ports_html = "".join(
            f'<div class="kv"><span class="k">{p.device}</span>'
            f'<span class="v" style="font-size:11px">{p.description}</span></div>'
            for p in candidate_ports())
        st.markdown(panel("SERIAL PORTS", ports_html or "No ports found.", "#38d6ff"),
                    unsafe_allow_html=True)

        age = state.last_rx_age()
        st.markdown(panel("LINK", kv_rows([
            ("Overall", overall, overall_colour),
            ("Last RX age", f"{age:.1f} s" if age is not None else "-", None),
            ("ACK status", "RECEIVED" if tx.ack_ok else
             "TIMEOUT" if tx.local_tx_ok else "-",
             "#2fe0a0" if tx.ack_ok else "#ffb224" if tx.local_tx_ok else None),
            ("Detected TX", state.tx.port or "-", None),
            ("Detected RX", state.rx.port or "-", None),
        ]), "#2fe0a0"), unsafe_allow_html=True)

    st.markdown("### TELEMETRY HISTORY")
    if state.history:
        head = "<tr>" + "".join(
            f"<th style='text-align:left;padding:6px 10px;color:#6d8399;"
            f"font-size:11px;letter-spacing:.1em;border-bottom:1px solid #16283f'>{h}</th>"
            for h in ("TIME", "SEQ", "TYPE", "CH1", "CH2", "CH3", "CH4",
                      "MESSAGE", "RSSI", "SNR", "ACK")) + "</tr>"
        body = []
        for row in list(state.history)[:100]:
            cells = [
                row.wall_time,
                str(row.sequence) if row.sequence is not None else "-",
                "MSG" if row.packet_type == "M" else "TLM",
                str(row.channels[0]) if row.channels[0] is not None else "-",
                str(row.channels[1]) if row.channels[1] is not None else "-",
                str(row.channels[2]) if row.channels[2] is not None else "-",
                str(row.channels[3]) if row.channels[3] is not None else "-",
                row.text or "-",
                f"{row.rssi}" if row.rssi is not None else "-",
                f"{row.snr:.2f}" if row.snr is not None else "-",
                row.ack,
            ]
            colour = "#2fe0a0" if row.ack == "OK" else "#ff4f60"
            tds = "".join(
                f"<td style='padding:5px 10px;border-bottom:1px solid #0f1e30;"
                f"font-family:Consolas,monospace;font-size:12px;"
                f"{'color:' + colour + ';' if i == 10 else 'color:#cfe0f0;'}'>"
                f"{c}</td>" for i, c in enumerate(cells))
            body.append(f"<tr>{tds}</tr>")
        st.markdown(
            "<div style='overflow:auto;max-height:340px;border:1px solid #16283f;"
            "border-radius:10px;background:#0a1424'>"
            f"<table style='width:100%;border-collapse:collapse'>{head}"
            + "".join(body) + "</table></div>", unsafe_allow_html=True)
    else:
        st.info("No received packets yet. Enter SEND,1500,1000,2000,1500 on the "
                "transmitter console or in the sidebar.")

    with st.expander("EVENT LOG", expanded=False):
        if state.events:
            for stamp, level, msg in list(state.events)[:120]:
                colour = {"ok": "#2fe0a0", "err": "#ff4f60",
                          "warn": "#ffb224"}.get(level, "#6d8399")
                st.markdown(
                    f'<div class="mono" style="font-size:12px">'
                    f'<span style="color:#43566a">[{stamp}]</span> '
                    f'<span style="color:{colour}">{msg}</span></div>',
                    unsafe_allow_html=True)
        else:
            st.caption("No events yet.")

# --------------------------------------------------------------------------- page 2
else:
    checks = diagnostics.evaluate(state, cfg.get("link_timeout_ms", 8000) / 1000)
    overall, overall_colour = diagnostics.overall(state, checks)

    st.markdown('<div class="gs-title">SYSTEM DIAGNOSTICS</div>', unsafe_allow_html=True)
    st.markdown('<div class="gs-sub">Fault isolation: USB, LoRa hardware, wireless '
                'link, acknowledgement</div>', unsafe_allow_html=True)
    st.markdown("<div style='height:10px'></div>", unsafe_allow_html=True)
    st.markdown(badge(overall, overall_colour), unsafe_allow_html=True)
    st.markdown("<div style='height:14px'></div>", unsafe_allow_html=True)

    groups = (
        ("TRANSMITTER", ("Transmitter USB / serial", "Transmitter firmware role",
                         "Transmitter LoRa / SPI")),
        ("RECEIVER", ("Receiver USB / serial", "Receiver firmware role",
                      "Receiver LoRa / SPI")),
        ("WIRELESS LINK", ("Packet reception", "Link freshness", "Acknowledgement")),
        ("DATA QUALITY", ("Invalid packets", "Duplicate packets",
                          "Packet loss (from sequence gaps)")),
    )
    for group, keys in groups:
        st.markdown(f"##### {group}")
        for c in checks:
            if c.label not in keys:
                continue
            remedy = ""
            if c.recommendation and c.state in (diagnostics.FAIL, diagnostics.WARN):
                remedy = (f'<div class="small" style="color:#ffb224;margin-top:4px">'
                          f'{c.recommendation}</div>')
            st.markdown(
                f'<div class="card" style="margin-bottom:8px;padding:10px 14px">'
                f'<div style="display:flex;justify-content:space-between">'
                f'<b>{c.label}</b>'
                f'<span style="color:{c.colour};font-weight:700">{c.state}</span></div>'
                f'<div class="small" style="margin-top:3px">{c.detail}</div>'
                f'{remedy}</div>', unsafe_allow_html=True)

    st.markdown("### RECOMMENDATIONS")
    recs = diagnostics.recommendations(checks)
    if recs:
        for r in recs:
            st.warning(r)
    else:
        st.success("No detectable faults. All measured checks pass.")

    st.markdown("### DETECTED HARDWARE")
    st.markdown(panel("SERIAL INTERFACES", kv_rows([
        ("Transmitter port", state.tx.port or "-", None),
        ("Transmitter role", "VERIFIED" if state.tx.role_ok else "UNVERIFIED",
         "#2fe0a0" if state.tx.role_ok else "#ffb224"),
        ("Receiver port", state.rx.port or "-", None),
        ("Receiver role", "VERIFIED" if state.rx.role_ok else "UNVERIFIED",
         "#2fe0a0" if state.rx.role_ok else "#ffb224"),
        ("Transmitter SX1278", "OK" if state.tx.radio_ok else "FAIL",
         "#2fe0a0" if state.tx.radio_ok else "#ff4f60"),
        ("Receiver SX1278", "OK" if state.rx.radio_ok else "FAIL",
         "#2fe0a0" if state.rx.radio_ok else "#ff4f60"),
    ]), "#38d6ff"), unsafe_allow_html=True)

    st.caption("Antenna continuity, RF output and module damage cannot be measured "
               "through the SX1278 register interface. If all software checks pass "
               "but no packet is received, inspect antennas, module seating and the "
               "3.3 V supply physically.")

# --------------------------------------------------------------------------- refresh
if cfg.get("auto_refresh", True):
    time.sleep(max(0.2, float(cfg.get("refresh_seconds", 0.6))))
    st.rerun()
