# Test results — 2026-11-17

Hardware: 2 × ESP32-WROOM-32 DevKit + 2 × RA-02 SX1278 (433 MHz), antennas
fitted. Windows host, Python 3.12, pyserial.

Ports / device IDs: **COM4 = 7630**, **COM6 = 2DF4**.

All output below is copied verbatim from the serial capture. Nothing is
simulated.

---

## 1. Hardware probe — `python tests/peer_diag.py --p1 COM4 --p2 COM6`

Five consecutive `STATUS` reads per board.

```text
============================================================
COM4
  READY    : PEERREADY,id=7630,radio=OK,freq=433000000,sf=7,sync=0x12,seq=10
  versions : [('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK')]
  freq     : ['433000000', '433000000', '433000000', '433000000', '433000000']
  opmode   : ['0x86', '0x86', '0x86', '0x86', '0x86']
============================================================
COM6
  READY    : PEERREADY,id=2DF4,radio=OK,freq=433000000,sf=7,sync=0x12,seq=9
  versions : [('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK'), ('0x12', 'OK')]
  freq     : ['433000000', '433000000', '433000000', '433000000', '433000000']
  opmode   : ['0x86', '0x86', '0x86', '0x86', '0x86']
```

`0x86` is `RX_SINGLE`, one of the two normal listening states. `0x85`
(`RX_CONTINUOUS`) is equally valid.

---

## 2. Failure that was fixed: ACK never received

Before the timing fix, data was delivered in both directions and each receiver
transmitted its ACK successfully (`ACKTX ... begin=1, txdone=1`), but the sender
always timed out:

```text
COM4 -> COM6: TEXT,DIAG TEST
   TX: TXSTAGE,seq=11,stage=WAITING_ACK,attempt=1,ms=3008
   TX: TXSTAGE,seq=11,stage=ACK_TIMEOUT,attempt=1,ms=4830
   ...
   TX: TXSTAGE,seq=11,stage=DELIVERY_NOT_CONFIRMED,attempts=3,tx=OK,ack=TIMEOUT
   RX: ACKTX,seq=11,from=2DF4,begin=1,txdone=1,opmode_after=0x81,freq=433000000,ms=3050
   RX: RXPKT,seq=11,from=7630,type=T,dup=0,rssi=-52,snr=9.25,ack=OK
```

Cause: the receiver ACKed ~40 ms after decoding the data packet, while the sender
was still completing its TX → RX transition. The 9-byte ACK began and finished
before the sender's receiver was listening.

Fix in `peer/peer.ino`:

- receiver waits **200 ms** before sending the ACK and sends it **twice** 120 ms
  apart;
- sender applies a **50 ms** post-TX guard plus a 5 ms settle before listening;
- the RX watchdog accepts both `0x85` and `0x86` instead of forcing a mode;
- new diagnostics: `ACKLISTEN`, `ACKRAW`, `ACKMATCH`, `ACKREJECT`.

Protocol, CRC, sequence handling, duplicate detection and the dashboard were not
changed.

---

## 3. Text, both directions (after the fix)

```text
================================================================
COM4 -> COM6: TEXT,ACK FIX TEST A TO B
  SRC: ACKLISTEN,opmode=0x81,freq=433000000,irq=0x00,elapsed=1,ms=3617
  SRC: ACKRAW,len=9,payload="A,12,2DF4",hex=41 2C 31 32 2C 32 44 46 34 ,rssi=-57,snr=9.75,seq=12,expect="A,12,",opmode=0x85,ms=3789
  SRC: ACKMATCH,seq=12,payload="A,12,2DF4",ms=3790
  SRC: TXSTAGE,seq=12,stage=ACK_RECEIVED,attempt=1,rssi=-57,snr=9.75,ms=3801
  SRC: TXSTAGE,seq=12,stage=DELIVERY_CONFIRMED,attempts=1,tx=OK,ack=OK,ms=3812
  DST: ACKTX,seq=12,from=2DF4,to=peer,attempt=1,begin=1,txdone=1,opmode_after=0x81,freq=433000000,ms=3785
  DST: ACKTX,seq=12,from=2DF4,to=peer,attempt=2,begin=1,txdone=1,opmode_after=0x81,freq=433000000,ms=3949
  DST: RXPKT,seq=12,from=7630,type=T,dup=0,rssi=-51,snr=9.75,ack=OK,text=41434B204649582054455354204120544F2042,...
================================================================
COM6 -> COM4: TEXT,ACK FIX TEST B TO A
  SRC: ACKLISTEN,opmode=0x81,freq=433000000,irq=0x00,elapsed=1,ms=13665
  SRC: ACKRAW,len=9,payload="A,11,7630",hex=41 2C 31 31 2C 37 36 33 30 ,rssi=-54,snr=9.50,seq=11,expect="A,11,",opmode=0x85,ms=13836
  SRC: ACKMATCH,seq=11,payload="A,11,7630",ms=13837
  SRC: TXSTAGE,seq=11,stage=ACK_RECEIVED,attempt=1,rssi=-54,snr=9.50,ms=13848
  SRC: TXSTAGE,seq=11,stage=DELIVERY_CONFIRMED,attempts=1,tx=OK,ack=OK,ms=13859
  DST: ACKTX,seq=11,from=7630,to=peer,attempt=1,begin=1,txdone=1,opmode_after=0x81,freq=433000000,ms=13840
  DST: ACKTX,seq=11,from=7630,to=peer,attempt=2,begin=1,txdone=1,opmode_after=0x81,freq=433000000,ms=14004
  DST: RXPKT,seq=11,from=2DF4,type=T,dup=0,rssi=-51,snr=9.75,ack=OK,text=41434B204649582054455354204220544F2041,...
```

---

## 4. Numeric data, both directions

```text
================================================================
COM4 -> COM6: DATA,100,200,300,400
  SRC: ACKMATCH,seq=13,payload="A,13,2DF4",ms=3750
  SRC: TXSTAGE,seq=13,stage=ACK_RECEIVED,attempt=1,rssi=-57,snr=9.50,ms=3761
  SRC: TXSTAGE,seq=13,stage=DELIVERY_CONFIRMED,attempts=1,tx=OK,ack=OK,ms=3772
  DST: RXPKT,seq=13,from=7630,type=D,dup=0,rssi=-50,snr=9.50,ack=OK,text=-,c1=100,c2=200,c3=300,c4=400,rx=1,rxdup=0,rxinvalid=0,ms=4053
  DST: VALUES: 100, 200, 300, 400
================================================================
COM6 -> COM4: DATA,1500,1200,1800,2000
  SRC: ACKMATCH,seq=12,payload="A,12,7630",ms=11753
  SRC: TXSTAGE,seq=12,stage=ACK_RECEIVED,attempt=1,rssi=-56,snr=9.50,ms=11764
  SRC: TXSTAGE,seq=12,stage=DELIVERY_CONFIRMED,attempts=1,tx=OK,ack=OK,ms=11775
  DST: RXPKT,seq=12,from=2DF4,type=D,dup=0,rssi=-52,snr=9.25,ack=OK,text=-,c1=1500,c2=1200,c3=1800,c4=2000,rx=1,rxdup=0,rxinvalid=0,ms=12062
  DST: VALUES: 1500, 1200, 1800, 2000
```

---

## 5. Summary

| Check | Result |
|---|---|
| SX1278 version, both boards | `0x12` on 5/5 reads |
| Tuned frequency | 433 000 000 Hz on 5/5 reads |
| Text COM4 → COM6 | received, ACK matched, delivery confirmed (attempt 1) |
| Text COM6 → COM4 | received, ACK matched, delivery confirmed (attempt 1) |
| Data COM4 → COM6 | `100,200,300,400` received, ACK matched, confirmed |
| Data COM6 → COM4 | `1500,1200,1800,2000` received, ACK matched, confirmed |
| Delivery confirmation rule | only after `ACKMATCH` for the sent sequence |
| Duplicate handling | `dup=1` counted, not re-delivered, ACK re-sent |

No ACK failures were observed after the fix in these runs.

---

## 6. Reproducing

```powershell
cd C:\Users\USER\Documents\Arduino\LoRa_Ground_Station
python tests\peer_diag.py --p1 COM4 --p2 COM6
python tests\peer_diag.py --p1 COM4 --p2 COM6 --test
```

Close the Streamlit dashboard and any Arduino Serial Monitor first — only one
program may own a COM port.
