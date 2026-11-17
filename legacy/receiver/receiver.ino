/*
 * receiver.ino - LoRa ground-station receiver with acknowledgement
 * ---------------------------------------------------------------
 * ROLE      : RECEIVER / GROUND STATION (second COM port)
 * HARDWARE  : ESP32 + RA-02 (SX1278) 433 MHz, no joysticks.
 *
 * Receives : DAT,<seq>,<T|M>,<c1>,<c2>,<c3>,<c4>,<hex-text>
 * Sends    : ACK,<seq>   (only after a fully validated packet)
 *
 * Serial output (115200):
 *   RXREADY,...                 boot / role
 *   RXSTAT,...                  one line per accepted packet (dashboard)
 *   RXERR,...                   validation / radio errors
 *   RXHEARTBEAT,...              every 5 s
 *   human-readable block        every accepted packet and every timeout
 *
 * Nothing is invented: no packet means NO PACKET RECEIVED, and RSSI/SNR are
 * the SX1278 measurements of the actual received packet.
 *
 * Wiring: SCK=18 MISO=19 MOSI=23 NSS=5 RST=14 DIO0=26, 3.3 V.
 */

#include <SPI.h>
#include <LoRa.h>

#define FIRMWARE_ROLE "RECEIVER"

#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_SS    5
#define LORA_RST   14
#define LORA_DIO0  26

#define LORA_FREQUENCY   433E6
#define LORA_SF          7
#define LORA_BW          125E3
#define LORA_CR          5
#define LORA_SYNC_WORD   0x12
#define INIT_ATTEMPTS    3
#define ACK_TX_TIMEOUT_MS 800
#define LINK_TIMEOUT_MS   8000
#define RADIO_HEALTH_MS   3000

#define REG_IRQ_FLAGS    0x12
#define REG_OP_MODE      0x01
#define REG_VERSION      0x42
#define REG_FRF_MSB      0x06
#define REG_FRF_MID      0x07
#define REG_FRF_LSB      0x08
#define REG_SYNC_WORD    0x39
#define REG_MODEM_CFG1   0x1D
#define REG_MODEM_CFG2   0x1E
#define REG_MODEM_CFG3   0x26
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_RSSI_VALUE   0x1B
#define REG_PKT_RSSI     0x1A
#define REG_MODEM_STAT   0x18

bool radioReady = false;
uint32_t irqChanges = 0;
uint32_t rxDoneIrq = 0;
uint32_t crcErrorIrq = 0;
uint32_t headerValidIrq = 0;
uint32_t rxTimeoutIrq = 0;
uint32_t payloadDetected = 0;
int lastIrqValue = -1;
unsigned long lastIrqPoll = 0;
uint32_t receivedCount = 0;
uint32_t validCount = 0;
uint32_t invalidCount = 0;
uint32_t duplicateCount = 0;
uint32_t lostCount = 0;
uint32_t ackSentCount = 0;
uint32_t ackFailCount = 0;
uint32_t rearmCount = 0;
int32_t lastSeq = -1;
int32_t lastId = -1;
unsigned long lastPacketMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastHealthMs = 0;
bool everReceived = false;
uint8_t healthFails = 0;

// last packet for the human block / dashboard
uint32_t lastSequence = 0;
char lastType = '?';
uint16_t lastCh[4] = {0, 0, 0, 0};
String lastText;
int lastRssi = 0;
float lastSnr = 0.0f;
bool lastAckOk = false;
bool haveLast = false;

uint32_t ring[32];
uint8_t ringCount = 0;
uint8_t ringIdx = 0;

// ------------------------------------------------------------ raw SPI
uint8_t regRead(uint8_t addr) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(addr & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
  return v;
}

void regWrite(uint8_t addr, uint8_t value) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_SS, LOW);
  SPI.transfer(addr | 0x80);
  SPI.transfer(value);
  digitalWrite(LORA_SS, HIGH);
  SPI.endTransaction();
}

uint32_t readFrequency() {
  uint32_t frf = ((uint32_t)regRead(REG_FRF_MSB) << 16) |
                 ((uint32_t)regRead(REG_FRF_MID) << 8) |
                  (uint32_t)regRead(REG_FRF_LSB);
  return (uint32_t)((double)frf * 32000000.0 / 524288.0 + 0.5);
}

void printConfigFull() {
  uint8_t mc1 = regRead(REG_MODEM_CFG1);
  uint8_t mc2 = regRead(REG_MODEM_CFG2);
  uint8_t mc3 = regRead(REG_MODEM_CFG3);
  Serial.println("---------- LORA CONFIG READ BACK ----------");
  Serial.printf("  version     : 0x%02X %s\n", regRead(REG_VERSION),
                regRead(REG_VERSION) == 0x12 ? "OK" : "UNEXPECTED");
  Serial.printf("  opmode      : 0x%02X\n", regRead(REG_OP_MODE));
  Serial.printf("  frequency   : %lu Hz\n", (unsigned long)readFrequency());
  Serial.printf("  bandwidth   : code %u (7 = 125 kHz)\n", (mc1 >> 4) & 0x0F);
  Serial.printf("  coding rate : code %u (1 = 4/5)\n", (mc1 >> 1) & 0x07);
  Serial.printf("  header mode : %s\n", (mc1 & 1) ? "IMPLICIT" : "EXPLICIT");
  Serial.printf("  spreading   : SF%u\n", (mc2 >> 4) & 0x0F);
  Serial.printf("  CRC         : %s\n", (mc2 & 0x04) ? "ON" : "OFF");
  Serial.printf("  sync word   : 0x%02X\n", regRead(REG_SYNC_WORD));
  Serial.printf("  preamble    : %u symbols\n",
                ((uint16_t)regRead(REG_PREAMBLE_MSB) << 8) |
                 regRead(REG_PREAMBLE_LSB));
  Serial.printf("  AGC         : %s\n", (mc3 & 0x04) ? "ON" : "OFF");
  Serial.printf("  wideband RSSI: %d dBm\n", (int)regRead(REG_RSSI_VALUE) - 164);
  Serial.println("-------------------------------------------");
}

void printIrqDiag() {
  uint8_t irq = regRead(REG_IRQ_FLAGS);
  Serial.printf("RXDIAG,irq=0x%02X,rxdone=%lu,crc=%lu,header=%lu,timeout=%lu,"
                "changes=%lu,payloads=%lu,opmode=0x%02X,rssi=%d,pkt_rssi=%d,"
                "snr=%.2f,ms=%lu\n",
                irq,
                (unsigned long)rxDoneIrq, (unsigned long)crcErrorIrq,
                (unsigned long)headerValidIrq, (unsigned long)rxTimeoutIrq,
                (unsigned long)irqChanges, (unsigned long)payloadDetected,
                regRead(REG_OP_MODE), (int)regRead(REG_RSSI_VALUE) - 164,
                (int)regRead(REG_PKT_RSSI) - 164,
                ((int8_t)regRead(0x19)) * 0.25f, millis());
}

void resetRadioHardware() {
  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, HIGH);
  delay(5);
  digitalWrite(LORA_RST, LOW);
  delay(30);
  digitalWrite(LORA_RST, HIGH);
  delay(120);
}

bool initRadio(const char *why) {
  Serial.printf("LoRa init (%s): %u attempts\n", why, INIT_ATTEMPTS);
  for (uint8_t attempt = 1; attempt <= INIT_ATTEMPTS; attempt++) {
    Serial.printf("  attempt %u/%u ... ", attempt, INIT_ATTEMPTS);
    SPI.end();
    resetRadioHardware();
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    delay(20);

    uint8_t version = regRead(REG_VERSION);
    Serial.printf("version=0x%02X ", version);
    if (version != 0x12) {
      Serial.println("-> FAIL (check 3.3V, GND, NSS=5, MISO=19, SCK=18, MOSI=23)");
      delay(200);
      continue;
    }

    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    LoRa.setSPIFrequency(1000000);
    if (!LoRa.begin(LORA_FREQUENCY)) {
      Serial.println("-> FAIL LoRa.begin()");
      delay(200);
      continue;
    }
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.receive();

    uint32_t f = readFrequency();
    if (f < 432000000UL || f > 434000000UL) {
      Serial.printf("-> FAIL tuning %lu Hz\n", (unsigned long)f);
      delay(200);
      continue;
    }
    Serial.println("-> OK (RX continuous, 433 MHz, sync 0x12)");
    return true;
  }
  return false;
}

// ------------------------------------------------------------ output helpers
String hexDecode(const String &s) {
  String out;
  for (size_t i = 0; i + 1 < s.length(); i += 2) {
    char buf[3] = {s[i], s[i + 1], 0};
    out += (char)strtol(buf, nullptr, 16);
  }
  return out;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexValid(const String &s) {
  if (s.length() % 2 != 0) return false;
  for (size_t i = 0; i < s.length(); i++) {
    if (hexNibble(s[i]) < 0) return false;
  }
  return true;
}

void printHeader() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("            LORA GROUND STATION");
  Serial.println("========================================");
  Serial.println();
}

const char *signalQuality(int rssi) {
  if (rssi > -95) return "EXCELLENT";
  if (rssi > -105) return "GOOD";
  if (rssi > -115) return "FAIR";
  return "WEAK";
}

void printBlock(bool ackOk) {
  printHeader();
  Serial.println("GROUND STATION STATUS: INITIALIZED");
  Serial.println();
  Serial.println("HARDWARE STATUS:");
  Serial.println("ESP32: CONNECTED");
  Serial.println("SPI: CHECKED");
  Serial.printf("LORA: %s\n", radioReady ? "INITIALIZED" : "FAILED");
  Serial.printf("FREQUENCY: %lu Hz (433 MHz)\n", (unsigned long)readFrequency());
  Serial.println("SYNC WORD: 0x12");
  Serial.println();
  Serial.println("WIRELESS COMMUNICATION:");
  Serial.println("PACKET STATUS: RECEIVED");
  Serial.printf("PACKET SEQUENCE: %03lu\n", (unsigned long)lastSequence);
  Serial.printf("PACKET TYPE: %s\n", lastType == 'M' ? "MESSAGE" : "TELEMETRY");
  Serial.println();
  Serial.println("RECEIVED TELEMETRY:");
  Serial.printf("CHANNEL 1: %u\nCHANNEL 2: %u\nCHANNEL 3: %u\nCHANNEL 4: %u\n",
                lastCh[0], lastCh[1], lastCh[2], lastCh[3]);
  if (lastText.length()) {
    Serial.printf("MESSAGE: %s\n", lastText.c_str());
  }
  Serial.println();
  Serial.println("SIGNAL INFORMATION:");
  Serial.printf("RSSI: %d dBm\n", lastRssi);
  Serial.printf("SNR: %.2f dB\n", lastSnr);
  Serial.printf("SIGNAL QUALITY: %s\n", signalQuality(lastRssi));
  Serial.println();
  Serial.println("PACKET STATISTICS:");
  Serial.printf("TOTAL PACKETS RECEIVED: %lu\n", (unsigned long)receivedCount);
  Serial.printf("VALID PACKETS: %lu\n", (unsigned long)validCount);
  Serial.printf("INVALID PACKETS: %lu\n", (unsigned long)invalidCount);
  Serial.printf("DUPLICATE PACKETS: %lu\n", (unsigned long)duplicateCount);
  Serial.printf("PACKET LOSS: %lu\n", (unsigned long)lostCount);
  Serial.println();
  Serial.println("ACKNOWLEDGEMENT:");
  Serial.printf("ACK SENT TO TRANSMITTER: %s\n", ackOk ? "SUCCESS" : "FAILED");
  Serial.println();
  Serial.printf("GROUND STATION STATUS: %s\n",
                (radioReady && ackOk) ? "OPERATIONAL" : "DEGRADED");
  Serial.println();
}

void printNoPacket() {
  printHeader();
  Serial.println("GROUND STATION STATUS: INITIALIZED");
  Serial.println();
  Serial.println("HARDWARE STATUS:");
  Serial.println("ESP32: CONNECTED");
  Serial.println("SPI: CHECKED");
  Serial.printf("LORA: %s\n", radioReady ? "INITIALIZED" : "FAILED");
  Serial.printf("FREQUENCY: %lu Hz (433 MHz)\n", (unsigned long)readFrequency());
  Serial.println();
  Serial.println("WIRELESS COMMUNICATION:");
  Serial.println("PACKET STATUS: NO PACKET RECEIVED");
  Serial.println("RECOMMENDATION: enter a command on the transmitter Serial Monitor.");
  if (everReceived) {
    Serial.printf("LAST PACKET: sequence %lu, %lu ms ago\n",
                  (unsigned long)lastSequence, millis() - lastPacketMs);
  }
  Serial.println();
}

// ------------------------------------------------------------ packet handling
bool isDuplicate(uint32_t seq) {
  for (uint8_t i = 0; i < ringCount; i++) {
    if (ring[i] == seq) return true;
  }
  return false;
}

void remember(uint32_t seq) {
  ring[ringIdx] = seq;
  ringIdx = (ringIdx + 1) % 32;
  if (ringCount < 32) ringCount++;
}

void clearRing() {
  ringCount = 0;
  ringIdx = 0;
}

bool sendAck(uint32_t seq) {
  regWrite(REG_IRQ_FLAGS, 0xFF);
  if (!LoRa.beginPacket()) return false;
  LoRa.print("ACK," + String(seq));
  LoRa.endPacket(true);

  unsigned long start = millis();
  bool ok = false;
  while (millis() - start < ACK_TX_TIMEOUT_MS) {
    if (regRead(REG_IRQ_FLAGS) & 0x08) { ok = true; break; }
    delay(2);
  }
  regWrite(REG_IRQ_FLAGS, 0xFF);
  LoRa.receive();
  return ok;
}

void emitStatus(uint32_t seq, char type, bool ackOk, bool duplicate) {
  Serial.printf("RXSTAT,seq=%lu,type=%c,c1=%u,c2=%u,c3=%u,c4=%u,text=%s,"
                "rssi=%d,snr=%.2f,rxc=%lu,valid=%lu,invalid=%lu,dup=%lu,"
                "lost=%lu,ack=%s,ackok=%lu,ackfail=%lu,dupflag=%u,"
                "ms=%lu,role=RECEIVER\n",
                (unsigned long)seq, type,
                lastCh[0], lastCh[1], lastCh[2], lastCh[3],
                lastText.length() ? lastText.c_str() : "-",
                lastRssi, lastSnr,
                (unsigned long)receivedCount, (unsigned long)validCount,
                (unsigned long)invalidCount, (unsigned long)duplicateCount,
                (unsigned long)lostCount,
                ackOk ? "OK" : "FAIL",
                (unsigned long)ackSentCount, (unsigned long)ackFailCount,
                duplicate ? 1 : 0, millis());
}

bool parseTelemetry(const String &msg, uint32_t &seq, char &type,
                    uint16_t ch[4], String &text, const char **reason) {
  if (!msg.startsWith("DAT,")) { *reason = "unknown packet type"; return false; }

  String fields[8];
  uint8_t n = 0;
  int start = 0;
  for (int i = 0; i <= (int)msg.length(); i++) {
    if (i == (int)msg.length() || msg[i] == ',') {
      if (n < 8) fields[n] = msg.substring(start, i);
      n++;
      start = i + 1;
    }
  }
  if (n != 8) { *reason = "expected 8 comma-separated fields"; return false; }
  if (fields[0] != "DAT") { *reason = "bad header"; return false; }

  char *end = nullptr;
  unsigned long s = strtoul(fields[1].c_str(), &end, 10);
  if (!end || *end != 0) { *reason = "sequence is not numeric"; return false; }
  seq = (uint32_t)s;

  if (fields[2] != "T" && fields[2] != "M") {
    *reason = "type must be T or M"; return false;
  }
  type = fields[2][0];

  for (uint8_t i = 0; i < 4; i++) {
    end = nullptr;
    unsigned long v = strtoul(fields[3 + i].c_str(), &end, 10);
    if (!end || *end != 0) { *reason = "channel is not numeric"; return false; }
    if (v > 4095) { *reason = "channel out of range 0..4095"; return false; }
    ch[i] = (uint16_t)v;
  }

  text = "";
  if (fields[7] != "-") {
    if (!hexValid(fields[7])) { *reason = "text is not valid hex"; return false; }
    text = hexDecode(fields[7]);
    for (size_t i = 0; i < text.length(); i++) {
      if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126) {
        *reason = "text has non-printable characters"; return false;
      }
    }
    if (text.length() > 32) { *reason = "text too long"; return false; }
  }
  if (type == 'M' && text.length() == 0) {
    *reason = "message packet has empty text"; return false;
  }
  return true;
}

void handlePacket(const String &msg) {
  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  uint32_t seq = 0;
  char type = '?';
  uint16_t ch[4] = {0, 0, 0, 0};
  String text;
  const char *reason = "";

  if (!parseTelemetry(msg, seq, type, ch, text, &reason)) {
    invalidCount++;
    Serial.printf("RXERR,kind=INVALID,reason=%s,rssi=%d,snr=%.2f,invalid=%lu,ms=%lu\n",
                  reason, rssi, snr, (unsigned long)invalidCount, millis());
    Serial.printf("PACKET INVALID: %s (rssi %d dBm, snr %.2f dB)\n", reason, rssi, snr);
    // Do not ACK an invalid packet.
    return;
  }

  receivedCount++;
  lastRssi = rssi;
  lastSnr = snr;

  bool dup = isDuplicate(seq);
  if (dup) {
    duplicateCount++;
    // A duplicate is still acknowledged so the transmitter stops retrying.
    bool ackOk = sendAck(seq);
    Serial.printf("DUPLICATE PACKET seq=%lu acknowledged (%s)\n",
                  (unsigned long)seq, ackOk ? "OK" : "FAILED");
    emitStatus(seq, type, ackOk, true);
    return;
  }

  validCount++;
  remember(seq);
  lastSequence = seq;
  lastType = type;
  lastText = text;
  for (uint8_t i = 0; i < 4; i++) lastCh[i] = ch[i];
  haveLast = true;
  lastPacketMs = millis();
  everReceived = true;

  if (lastId >= 0 && seq > (uint32_t)lastId + 1) {
    lostCount += seq - (uint32_t)lastId - 1;
  } else if (lastId >= 0 && seq <= (uint32_t)lastId) {
    // Sequence went backwards: transmitter rebooted or sequence reset.
    Serial.printf("PACKET SEQUENCE RESET: %ld -> %lu (counters continue)\n",
                  (long)lastId, (unsigned long)seq);
  }
  lastId = (int32_t)seq;

  bool ackOk = sendAck(seq);
  if (ackOk) ackSentCount++; else ackFailCount++;
  lastAckOk = ackOk;

  Serial.printf("RXSTAGE,seq=%lu,stage=VALID,rssi=%d,snr=%.2f,ms=%lu\n",
                (unsigned long)seq, lastRssi, lastSnr, millis());
  Serial.printf("RXSTAGE,seq=%lu,stage=ACK_SENT,ack=%s,ms=%lu\n",
                (unsigned long)seq, ackOk ? "OK" : "FAIL", millis());

  printBlock(ackOk);
  if (lastText.length()) {
    Serial.printf("MESSAGE: %s\n", lastText.c_str());
  }
  emitStatus(seq, type, ackOk, false);
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  pinMode(LORA_DIO0, INPUT);

  printHeader();
  Serial.println("GROUND STATION STATUS: INITIALIZING");
  Serial.println("ROLE=RECEIVER");

  radioReady = initRadio("boot");
  if (radioReady) {
    Serial.println("RADIO=OK");
    printConfigFull();
  } else {
    Serial.println("RADIO=FAIL - retrying every 2 s");
  }

  Serial.printf("RXREADY,role=RECEIVER,freq=%lu,sf=%u,sync=0x%02X,radio=%s\n",
                (unsigned long)readFrequency(), LORA_SF, LORA_SYNC_WORD,
                radioReady ? "OK" : "FAIL");
  lastIrqValue = regRead(REG_IRQ_FLAGS);
  printIrqDiag();

  lastHeartbeatMs = millis();
  lastHealthMs = millis();
}

void loop() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'D' || c == 'd') {
      printConfigFull();
      printIrqDiag();
    }
  }

  if (!radioReady) {
    static unsigned long lastInit = 0;
    if (millis() - lastInit >= 2000) {
      lastInit = millis();
      radioReady = initRadio("recovery");
      if (radioReady) {
        Serial.println("RADIO=OK (recovered)");
        Serial.printf("RXREADY,role=RECEIVER,freq=%lu,sf=%u,sync=0x%02X,radio=OK\n",
                      (unsigned long)readFrequency(), LORA_SF, LORA_SYNC_WORD);
      }
    }
    delay(10);
    return;
  }

  int size = LoRa.parsePacket();
  if (size > 0) {
    payloadDetected++;
    String msg;
    msg.reserve(size);
    while (LoRa.available()) msg += (char)LoRa.read();
    Serial.printf("PACKET DETECTED len=%d rssi=%d snr=%.2f payload=\"%s\"\n",
                  size, LoRa.packetRssi(), LoRa.packetSnr(), msg.c_str());
    handlePacket(msg);
    LoRa.receive();
  } else {
    // Recover from a corrupted header leaving the radio in RX_SINGLE.
    static unsigned long lastRearm = 0;
    if (millis() - lastRearm >= 250) {
      lastRearm = millis();
      uint8_t op = regRead(REG_OP_MODE);
      if ((op & 0x80) && ((op & 0x07) != 5)) {
        regWrite(REG_IRQ_FLAGS, 0xFF);
        LoRa.receive();
        rearmCount++;
      }
    }
  }

  unsigned long now = millis();

  if (now - lastIrqPoll >= 100) {
    lastIrqPoll = now;
    uint8_t irq = regRead(REG_IRQ_FLAGS);
    if ((int)irq != lastIrqValue) {
      lastIrqValue = irq;
      irqChanges++;
      if (irq & 0x40) rxDoneIrq++;
      if (irq & 0x20) crcErrorIrq++;
      if (irq & 0x10) headerValidIrq++;
      if (irq & 0x80) rxTimeoutIrq++;
      Serial.printf("IRQ CHANGE 0x%02X%s%s%s%s%s\n", irq,
                    (irq & 0x40) ? " RxDone" : "",
                    (irq & 0x20) ? " CrcErr" : "",
                    (irq & 0x10) ? " HeaderValid" : "",
                    (irq & 0x80) ? " RxTimeout" : "",
                    (irq & 0x08) ? " TxDone" : "");
      printIrqDiag();
    }
  }

  if (now - lastHealthMs >= RADIO_HEALTH_MS) {
    lastHealthMs = now;
    uint8_t version = regRead(REG_VERSION);
    uint8_t op = regRead(REG_OP_MODE);
    uint32_t f = readFrequency();
    bool ok = (version == 0x12) && (op & 0x80) &&
              (f > 432000000UL && f < 434000000UL);
    if (!ok) {
      healthFails++;
      Serial.printf("RXERR,kind=RADIO,version=0x%02X,opmode=0x%02X,freq=%lu,"
                    "count=%u,ms=%lu\n", version, op, (unsigned long)f,
                    healthFails, now);
      if (healthFails >= 3) {
        Serial.println("RADIO LOST: reinitialising");
        healthFails = 0;
        radioReady = false;
      }
    } else {
      healthFails = 0;
    }
  }

  if (now - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = now;
    bool timeout = everReceived && (now - lastPacketMs > LINK_TIMEOUT_MS);
    if (!everReceived) {
      printNoPacket();
    } else if (timeout) {
      Serial.printf("COMMUNICATION TIMEOUT: no packet for %lu ms (last seq %lu)\n",
                    now - lastPacketMs, (unsigned long)lastSequence);
      printNoPacket();
    }
    Serial.printf("RXHEARTBEAT,rxc=%lu,valid=%lu,invalid=%lu,dup=%lu,lost=%lu,"
                  "ackok=%lu,ackfail=%lu,rearm=%lu,radio=%s,last_seq=%ld,"
                  "age=%ld,ms=%lu\n",
                  (unsigned long)receivedCount, (unsigned long)validCount,
                  (unsigned long)invalidCount, (unsigned long)duplicateCount,
                  (unsigned long)lostCount, (unsigned long)ackSentCount,
                  (unsigned long)ackFailCount, (unsigned long)rearmCount,
                  radioReady ? "OK" : "FAIL", (long)lastId,
                  everReceived ? (long)(now - lastPacketMs) : -1L, now);
  }
}
