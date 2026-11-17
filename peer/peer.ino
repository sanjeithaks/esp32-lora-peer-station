/*
 * peer.ino - symmetric bidirectional LoRa peer (ESP32 + RA-02 / SX1278)
 * ---------------------------------------------------------------------
 * Flash the SAME firmware to both boards. Each board can transmit and receive.
 *
 * Device identity is derived from the ESP32 MAC address (last 2 bytes), so the
 * two boards are automatically distinct; no role or ID configuration needed.
 *
 * Serial commands (115200, newline):
 *   TEXT,HELLO FROM BOARD 1      send a text message (1..40 printable chars)
 *   DATA,100,200,300,400         send four numeric values (0..4095)
 *   STATUS                       hardware / radio diagnostics
 *   HELP                         command list
 *   any other non-empty line     treated as TEXT
 *
 * LoRa packet:
 *   P,<seq>,<senderId>,<T|D>,<payload>,<crc16>
 *   T payload = hex-encoded text
 *   D payload = a;b;c;d
 * ACK:
 *   A,<seq>,<ackerId>
 *
 * Both boards listen in continuous RX whenever they are not transmitting.
 */

#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>

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
#define LORA_TX_POWER    2

#define TX_DONE_TIMEOUT_MS 1200
#define ACK_TIMEOUT_MS     1800
#define MAX_ATTEMPTS       3
#define MAX_TEXT           40
#define INIT_ATTEMPTS      3
#define RADIO_HEALTH_MS    5000

#define REG_IRQ_FLAGS 0x12
#define REG_OP_MODE   0x01
#define REG_VERSION   0x42
#define REG_FRF_MSB   0x06
#define REG_FRF_MID   0x07
#define REG_FRF_LSB   0x08

Preferences prefs;
bool radioReady = false;
String deviceId = "0000";
uint32_t sequenceNumber = 0;

uint32_t txAttempts = 0, txOkCount = 0, txFailCount = 0;
uint32_t ackOkCount = 0, ackTimeoutCount = 0;
uint32_t rxCount = 0, rxInvalid = 0, rxDup = 0;
int lastRssi = 0;
float lastSnr = 0.0f;
uint32_t rearmCount = 0;

void logRadioCfg(const char *tag) {
  uint32_t frf = ((uint32_t)regRead(REG_FRF_MSB) << 16) |
                 ((uint32_t)regRead(REG_FRF_MID) << 8) |
                  (uint32_t)regRead(REG_FRF_LSB);
  Serial.printf("RADIOCFG,tag=%s,version=0x%02X,freq=%lu,frf=0x%06lX,"
                "opmode=0x%02X,mc1=0x%02X,mc2=0x%02X,mc3=0x%02X,"
                "sync=0x%02X,rearm=%lu,ms=%lu\n",
                tag, regRead(REG_VERSION), (unsigned long)readFrequency(),
                (unsigned long)frf, regRead(REG_OP_MODE),
                regRead(0x1D), regRead(0x1E), regRead(0x26),
                regRead(0x39), (unsigned long)rearmCount, millis());
}

String inputLine;
bool inputReady = false;

struct Seen { char id[5]; uint32_t seq; bool used; };
Seen seen[16];
uint8_t seenIdx = 0;

// ------------------------------------------------------------ SPI / radio
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
  Serial.printf("RADIORESET,why=%s,ms=%lu\n", why, millis());
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
      Serial.println("-> FAIL (3.3V / GND / SPI 18,19,23 / NSS 5 / RST 14)");
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
    LoRa.setTxPower(LORA_TX_POWER, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.receive();

    uint32_t f = readFrequency();
    if (f < 432000000UL || f > 434000000UL) {
      Serial.printf("-> FAIL tuning %lu Hz\n", (unsigned long)f);
      delay(200);
      continue;
    }
    Serial.println("-> OK (433 MHz, SF7, sync 0x12, RX continuous)");
    logRadioCfg(why);
    return true;
  }
  return false;
}

// Verify version and tuning, re-initialise once if needed.
bool ensureTuned(const char *when) {
  logRadioCfg(when);
  uint8_t v = regRead(REG_VERSION);
  uint32_t f = readFrequency();
  if (v == 0x12 && f > 432000000UL && f < 434000000UL) return true;
  Serial.printf("TUNING INVALID before %s: version=0x%02X freq=%lu -> reinit\n",
                when, v, (unsigned long)f);
  radioReady = initRadio("pretx-retry");
  logRadioCfg("after-reinit");
  v = regRead(REG_VERSION);
  f = readFrequency();
  return radioReady && v == 0x12 && f > 432000000UL && f < 434000000UL;
}

// ------------------------------------------------------------ helpers
uint16_t crc16(const String &s) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < s.length(); i++) {
    crc ^= (uint16_t)(uint8_t)s[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

String hexEncode(const String &s) {
  static const char *d = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    out += d[c >> 4];
    out += d[c & 0x0F];
  }
  return out;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexDecode(const String &s, String &out) {
  out = "";
  if (s.length() % 2) return false;
  for (size_t i = 0; i < s.length(); i += 2) {
    int hi = hexNibble(s[i]), lo = hexNibble(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out += (char)((hi << 4) | lo);
  }
  return true;
}

void printHelp() {
  Serial.println();
  Serial.println("========== LORA PEER ==========");
  Serial.printf("DEVICE ID: %s\n", deviceId.c_str());
  Serial.println("TEXT,<message>        send text (1..40 chars)");
  Serial.println("DATA,<c1>,<c2>,<c3>,<c4>   send numbers 0..4095");
  Serial.println("STATUS                diagnostics");
  Serial.println("HELP                  this list");
  Serial.println("Any other line is sent as TEXT.");
  Serial.println("===============================");
}

void printStatus() {
  uint8_t version = regRead(REG_VERSION);
  uint8_t op = regRead(REG_OP_MODE);
  uint32_t f = readFrequency();
  Serial.println();
  Serial.println("========== PEER STATUS ==========");
  Serial.printf("DEVICE ID: %s\n", deviceId.c_str());
  Serial.printf("SX1278 VERSION: 0x%02X %s\n", version,
                version == 0x12 ? "OK" : "UNEXPECTED");
  Serial.printf("LORA: %s\n", radioReady ? "INITIALIZED" : "FAILED");
  Serial.printf("OPMODE: 0x%02X\n", op);
  Serial.printf("FREQUENCY: %lu Hz\n", (unsigned long)f);
  Serial.printf("SYNC WORD: 0x%02X\n", regRead(0x39));
  Serial.printf("SF / BW / CR: SF%u / 125 kHz / 4/%u\n", LORA_SF, LORA_CR);
  Serial.printf("CRC: %s\n", (regRead(0x1E) & 0x04) ? "ON" : "OFF");
  Serial.printf("TX: %lu attempted, %lu ok, %lu failed\n",
                (unsigned long)txAttempts, (unsigned long)txOkCount,
                (unsigned long)txFailCount);
  Serial.printf("ACK: %lu received, %lu timeout\n",
                (unsigned long)ackOkCount, (unsigned long)ackTimeoutCount);
  Serial.printf("RX: %lu packets, %lu invalid, %lu duplicates\n",
                (unsigned long)rxCount, (unsigned long)rxInvalid,
                (unsigned long)rxDup);
  Serial.printf("SEQUENCE: %lu\n", (unsigned long)sequenceNumber);
  Serial.printf("LAST RSSI: %d dBm\n", lastRssi);
  Serial.printf("LAST SNR: %.2f dB\n", lastSnr);
  Serial.println("=================================");
}

// ------------------------------------------------------------ transmit
bool transmitPayload(const String &payload) {
  regWrite(REG_IRQ_FLAGS, 0xFF);
  if (!LoRa.beginPacket()) return false;
  LoRa.print(payload);
  LoRa.endPacket(true);
  unsigned long start = millis();
  while (millis() - start < TX_DONE_TIMEOUT_MS) {
    if (regRead(REG_IRQ_FLAGS) & 0x08) {
      regWrite(REG_IRQ_FLAGS, 0xFF);
      LoRa.idle();
      logRadioCfg("after-tx");
      return true;
    }
    delay(2);
  }
  regWrite(REG_IRQ_FLAGS, 0xFF);
  LoRa.idle();
  logRadioCfg("tx-timeout");
  return false;
}

bool waitForAck(uint32_t seq, unsigned long timeoutMs, int &rssi, float &snr) {
  // Guard: give the SX1278 time to switch from TX to RX before listening.
  delay(50);
  LoRa.receive();
  delay(5);
  logRadioCfg("ack-listen");
  String prefix = "A," + String(seq) + ",";
  unsigned long start = millis();
  unsigned long lastRearm = 0;
  unsigned long lastStateLog = 0;
  while (millis() - start < timeoutMs) {
    int size = LoRa.parsePacket();
    if (size > 0) {
      String msg;
      msg.reserve(size);
      while (LoRa.available()) msg += (char)LoRa.read();
      rssi = LoRa.packetRssi();
      snr = LoRa.packetSnr();
      LoRa.receive();
      String hex;
      for (size_t i = 0; i < msg.length(); i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", (uint8_t)msg[i]);
        hex += b;
      }
      Serial.printf("ACKRAW,len=%d,payload=\"%s\",hex=%s,rssi=%d,snr=%.2f,"
                    "seq=%lu,expect=\"%s\",opmode=0x%02X,ms=%lu\n",
                    size, msg.c_str(), hex.c_str(), rssi, snr,
                    (unsigned long)seq, prefix.c_str(),
                    regRead(REG_OP_MODE), millis());
      if (msg.startsWith(prefix)) {
        Serial.printf("ACKMATCH,seq=%lu,payload=\"%s\",ms=%lu\n",
                      (unsigned long)seq, msg.c_str(), millis());
        return true;
      }
      Serial.printf("ACKREJECT,payload=\"%s\",expected=\"%s\"\n",
                    msg.c_str(), prefix.c_str());
    } else {
      if (millis() - lastStateLog >= 500) {
        lastStateLog = millis();
        Serial.printf("ACKLISTEN,opmode=0x%02X,freq=%lu,irq=0x%02X,elapsed=%lu,ms=%lu\n",
                      regRead(REG_OP_MODE), (unsigned long)readFrequency(),
                      regRead(REG_IRQ_FLAGS), millis() - start, millis());
      }
      if (millis() - lastRearm >= 20) {
        lastRearm = millis();
        uint8_t op = regRead(REG_OP_MODE);
        uint8_t mode = op & 0x07;
        // 0x05 continuous and 0x06 single are both valid listening states.
        if ((op & 0x80) && mode != 5 && mode != 6) {
          regWrite(REG_IRQ_FLAGS, 0xFF);
          LoRa.receive();
          rearmCount++;
          if (rearmCount % 20 == 1) {
            Serial.printf("RXREARM,where=ack,opmode=0x%02X,count=%lu,ms=%lu\n",
                          op, (unsigned long)rearmCount, millis());
          }
        }
      }
    }
    delay(1);
  }
  return false;
}

void sendPeer(char type, const String &payload, const String &textDisplay,
              int c1, int c2, int c3, int c4) {
  sequenceNumber++;
  prefs.putULong("seq", sequenceNumber);
  String body = "P," + String(sequenceNumber) + "," + deviceId + "," +
                String(type) + "," + payload;
  uint16_t crc = crc16(body);
  char crcHex[5];
  snprintf(crcHex, sizeof(crcHex), "%04X", crc);
  String packet = body + "," + crcHex;

  Serial.println();
  Serial.println("========== TRANSMIT ==========");
  Serial.printf("DEVICE: %s\n", deviceId.c_str());
  Serial.printf("TYPE: %s\n", type == 'D' ? "NUMERIC" : "TEXT");
  Serial.printf("SEQUENCE: %lu\n", (unsigned long)sequenceNumber);
  if (type == 'D') {
    Serial.printf("CHANNEL 1: %d\nCHANNEL 2: %d\nCHANNEL 3: %d\nCHANNEL 4: %d\n",
                  c1, c2, c3, c4);
  } else {
    Serial.printf("MESSAGE: %s\n", textDisplay.c_str());
  }
  Serial.printf("PACKET: %s\n", packet.c_str());
  Serial.printf("TXBEGIN,seq=%lu,id=%s,type=%c,text=%s,c1=%d,c2=%d,c3=%d,"
                "c4=%d,packet=%s\n",
                (unsigned long)sequenceNumber, deviceId.c_str(), type,
                type == 'T' ? hexEncode(textDisplay).c_str() : "-",
                c1, c2, c3, c4, packet.c_str());
  Serial.printf("TXSTAGE,seq=%lu,stage=STARTED,ms=%lu\n",
                (unsigned long)sequenceNumber, millis());

  if (!ensureTuned("pretx")) {
    Serial.println("TX ABORTED: radio is not tuned to 433 MHz");
    Serial.printf("TXSTAGE,seq=%lu,stage=TRANSMISSION_FAILED,tuning=INVALID,ms=%lu\n",
                  (unsigned long)sequenceNumber, millis());
    Serial.printf("PKTSTAT,seq=%lu,id=%s,type=%c,tx=FAIL,ack=TIMEOUT,"
                  "attempts=0,ms=%lu\n",
                  (unsigned long)sequenceNumber, deviceId.c_str(), type, millis());
    return;
  }

  bool txOkAny = false, ackOk = false;
  int ackRssi = 0;
  float ackSnr = 0;
  uint8_t attempts = 0;

  if (!radioReady) {
    Serial.println("LORA TX: FAILED (radio not initialized)");
    Serial.printf("TXSTAGE,seq=%lu,stage=TRANSMISSION_FAILED,ms=%lu\n",
                  (unsigned long)sequenceNumber, millis());
    Serial.printf("PKTSTAT,seq=%lu,id=%s,type=%c,tx=FAIL,ack=TIMEOUT,"
                  "attempts=0,ms=%lu\n",
                  (unsigned long)sequenceNumber, deviceId.c_str(), type, millis());
    return;
  }

  for (uint8_t attempt = 1; attempt <= MAX_ATTEMPTS && !ackOk; attempt++) {
    attempts = attempt;
    txAttempts++;
    Serial.printf("ATTEMPT %u/%u ... ", attempt, MAX_ATTEMPTS);
    bool ok = transmitPayload(packet);
    Serial.println(ok ? "TX COMPLETE" : "TX FAILED");
    Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, ok ? "TX_COMPLETE" : "TX_FAILED",
                  attempt, millis());
    if (ok) { txOkCount++; txOkAny = true; }
    else { txFailCount++; if (!radioReady) break; }

    Serial.println("WAITING FOR ACK...");
    Serial.printf("TXSTAGE,seq=%lu,stage=WAITING_ACK,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, attempt, millis());
    ackOk = waitForAck(sequenceNumber, ACK_TIMEOUT_MS, ackRssi, ackSnr);
    if (ackOk) {
      ackOkCount++;
      Serial.printf("ACK RECEIVED (attempt %u, RSSI %d dBm, SNR %.2f dB)\n",
                    attempt, ackRssi, ackSnr);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_RECEIVED,attempt=%u,rssi=%d,"
                    "snr=%.2f,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, ackRssi, ackSnr, millis());
    } else {
      ackTimeoutCount++;
      Serial.printf("ACK TIMEOUT (attempt %u)\n", attempt);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_TIMEOUT,attempt=%u,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, millis());
    }
  }

  const char *finalStage = ackOk ? "DELIVERY_CONFIRMED" :
      (txOkAny ? "DELIVERY_NOT_CONFIRMED" : "TRANSMISSION_FAILED");
  Serial.println();
  if (ackOk) {
    Serial.println("FINAL: DELIVERY CONFIRMED");
    Serial.printf("MATCHING ACK FOR SEQUENCE %lu RECEIVED\n",
                  (unsigned long)sequenceNumber);
  } else if (txOkAny) {
    Serial.println("FINAL: DELIVERY NOT CONFIRMED (ACK timeout)");
  } else {
    Serial.println("FINAL: TRANSMISSION FAILED");
  }
  Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempts=%u,tx=%s,ack=%s,ms=%lu\n",
                (unsigned long)sequenceNumber, finalStage, attempts,
                txOkAny ? "OK" : "FAIL", ackOk ? "OK" : "TIMEOUT", millis());
  Serial.printf("PKTSTAT,seq=%lu,id=%s,type=%c,tx=%s,ack=%s,attempts=%u,"
                "rssi=%d,snr=%.2f,txok=%lu,txfail=%lu,ackok=%lu,ackto=%lu,"
                "rx=%lu,rxdup=%lu,rxinvalid=%lu,ms=%lu\n",
                (unsigned long)sequenceNumber, deviceId.c_str(), type,
                txOkAny ? "OK" : "FAIL", ackOk ? "OK" : "TIMEOUT", attempts,
                ackRssi, ackSnr,
                (unsigned long)txOkCount, (unsigned long)txFailCount,
                (unsigned long)ackOkCount, (unsigned long)ackTimeoutCount,
                (unsigned long)rxCount, (unsigned long)rxDup,
                (unsigned long)rxInvalid, millis());
  Serial.println("==============================");
  Serial.println("ENTER MESSAGE OR COMMAND:");
  LoRa.receive();
}

// ------------------------------------------------------------ receive
bool isDuplicate(const String &sender, uint32_t seq) {
  for (uint8_t i = 0; i < 16; i++) {
    if (seen[i].used && seen[i].seq == seq &&
        strncmp(seen[i].id, sender.c_str(), 4) == 0) return true;
  }
  return false;
}

void remember(const String &sender, uint32_t seq) {
  strncpy(seen[seenIdx].id, sender.c_str(), 4);
  seen[seenIdx].id[4] = 0;
  seen[seenIdx].seq = seq;
  seen[seenIdx].used = true;
  seenIdx = (seenIdx + 1) % 16;
}

bool sendAck(uint32_t seq) {
  logRadioCfg("ack-before");
  // Let the sender finish its TX -> RX switch before the ACK arrives.
  delay(200);
  String ack = "A," + String(seq) + "," + deviceId;
  bool anyOk = false;
  for (uint8_t n = 0; n < 2; n++) {
    regWrite(REG_IRQ_FLAGS, 0xFF);
    bool beginOk = LoRa.beginPacket();
    bool txDone = false;
    if (beginOk) {
      LoRa.print(ack);
      LoRa.endPacket(true);
      unsigned long start = millis();
      while (millis() - start < 600) {
        if (regRead(REG_IRQ_FLAGS) & 0x08) { txDone = true; break; }
        delay(2);
      }
    }
    Serial.printf("ACKTX,seq=%lu,from=%s,to=peer,attempt=%u,begin=%u,txdone=%u,"
                  "opmode_after=0x%02X,freq=%lu,ms=%lu\n",
                  (unsigned long)seq, deviceId.c_str(), n + 1,
                  beginOk ? 1 : 0, txDone ? 1 : 0, regRead(REG_OP_MODE),
                  (unsigned long)readFrequency(), millis());
    if (txDone) anyOk = true;
    regWrite(REG_IRQ_FLAGS, 0xFF);
    delay(120);
  }
  LoRa.receive();
  logRadioCfg("ack-after");
  return anyOk;
}

void handleReceived(const String &msg, int rssi, float snr) {
  // P,<seq>,<sender>,<T|D>,<payload>,<crc>
  String f[6];
  uint8_t n = 0;
  int start = 0;
  for (int i = 0; i <= (int)msg.length(); i++) {
    if (i == (int)msg.length() || msg[i] == ',') {
      if (n < 6) f[n] = msg.substring(start, i);
      n++;
      start = i + 1;
    }
  }
  if (n != 6 || f[0] != "P") {
    rxInvalid++;
    Serial.printf("RXERR,reason=format,rssi=%d,snr=%.2f,ms=%lu\n",
                  rssi, snr, millis());
    return;
  }
  char *end = nullptr;
  unsigned long seq = strtoul(f[1].c_str(), &end, 10);
  if (!end || *end) { rxInvalid++; Serial.println("RXERR,reason=sequence"); return; }
  String sender = f[2];
  if (sender.length() != 4) { rxInvalid++; Serial.println("RXERR,reason=sender"); return; }
  char type = f[3].length() ? f[3][0] : '?';
  if (type != 'T' && type != 'D') {
    rxInvalid++; Serial.println("RXERR,reason=type"); return;
  }
  String body = "P," + String(seq) + "," + sender + "," + String(type) + "," + f[4];
  uint16_t crc = crc16(body);
  char expect[5];
  snprintf(expect, sizeof(expect), "%04X", crc);
  if (!f[5].equalsIgnoreCase(expect)) {
    rxInvalid++;
    Serial.printf("RXERR,reason=crc,rssi=%d,snr=%.2f,ms=%lu\n", rssi, snr, millis());
    return;
  }

  bool dup = isDuplicate(sender, (uint32_t)seq);
  if (dup) {
    rxDup++;
    sendAck((uint32_t)seq);   // ack the retry so the sender stops
    Serial.printf("RXPKT,seq=%lu,from=%s,type=%c,dup=1,rssi=%d,snr=%.2f,"
                  "ack=OK,ms=%lu\n",
                  (unsigned long)seq, sender.c_str(), type, rssi, snr, millis());
    return;
  }

  rxCount++;
  remember(sender, (uint32_t)seq);

  int c1 = 0, c2 = 0, c3 = 0, c4 = 0;
  String text = "";
  String shown = "";
  bool valid = true;
  if (type == 'T') {
    if (!hexDecode(f[4], text) || text.length() == 0 || text.length() > MAX_TEXT) {
      valid = false;
    } else {
      for (size_t i = 0; i < text.length(); i++) {
        if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126) valid = false;
      }
      shown = "MESSAGE: " + text;
    }
  } else {
    int s1 = f[4].indexOf(';');
    int s2 = f[4].indexOf(';', s1 + 1);
    int s3 = f[4].indexOf(';', s2 + 1);
    if (s1 < 0 || s2 < 0 || s3 < 0) {
      valid = false;
    } else {
      c1 = f[4].substring(0, s1).toInt();
      c2 = f[4].substring(s1 + 1, s2).toInt();
      c3 = f[4].substring(s2 + 1, s3).toInt();
      c4 = f[4].substring(s3 + 1).toInt();
      if (c1 < 0 || c1 > 4095 || c2 < 0 || c2 > 4095 ||
          c3 < 0 || c3 > 4095 || c4 < 0 || c4 > 4095) valid = false;
      shown = "VALUES: " + String(c1) + ", " + String(c2) + ", " +
              String(c3) + ", " + String(c4);
    }
  }
  if (!valid) {
    rxInvalid++;
    Serial.printf("RXERR,reason=payload,rssi=%d,snr=%.2f,ms=%lu\n", rssi, snr, millis());
    return;
  }

  bool ackOk = sendAck((uint32_t)seq);
  lastRssi = rssi;
  lastSnr = snr;
  Serial.println();
  Serial.println("========== PACKET RECEIVED ==========");
  Serial.printf("FROM: %s\n", sender.c_str());
  Serial.printf("SEQUENCE: %lu\n", (unsigned long)seq);
  Serial.printf("TYPE: %s\n", type == 'D' ? "NUMERIC" : "TEXT");
  Serial.println(shown);
  Serial.printf("RSSI: %d dBm\n", rssi);
  Serial.printf("SNR: %.2f dB\n", snr);
  Serial.printf("ACK: %s\n", ackOk ? "SENT OK" : "FAILED");
  Serial.println("=====================================");
  Serial.printf("RXPKT,seq=%lu,from=%s,type=%c,dup=0,rssi=%d,snr=%.2f,ack=%s,"
                "text=%s,c1=%d,c2=%d,c3=%d,c4=%d,rx=%lu,rxdup=%lu,"
                "rxinvalid=%lu,ms=%lu\n",
                (unsigned long)seq, sender.c_str(), type, rssi, snr,
                ackOk ? "OK" : "FAIL",
                type == 'T' ? hexEncode(text).c_str() : "-",
                c1, c2, c3, c4, (unsigned long)rxCount, (unsigned long)rxDup,
                (unsigned long)rxInvalid, millis());
}

void serviceRadio() {
  if (!radioReady) return;
  int size = LoRa.parsePacket();
  if (size > 0) {
    String msg;
    msg.reserve(size);
    while (LoRa.available()) msg += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();
    LoRa.receive();
    if (msg.startsWith("A,")) return;   // ACK for a send in progress
    handleReceived(msg, rssi, snr);
  } else {
    static unsigned long lastRearm = 0;
    if (millis() - lastRearm >= 20) {
      lastRearm = millis();
      uint8_t op = regRead(REG_OP_MODE);
      uint8_t mode = op & 0x07;
      if ((op & 0x80) && mode != 5 && mode != 6) {
        regWrite(REG_IRQ_FLAGS, 0xFF);
        LoRa.receive();
        rearmCount++;
        if (rearmCount % 20 == 1) {
          Serial.printf("RXREARM,where=idle,opmode=0x%02X,count=%lu,ms=%lu\n",
                        op, (unsigned long)rearmCount, millis());
        }
      }
    }
  }
}

// ------------------------------------------------------------ commands
void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") { printHelp(); return; }
  if (upper == "STATUS") { printStatus(); return; }

  if (upper.startsWith("TEXT,")) {
    String text = line.substring(5);
    if (text.length() < 1 || text.length() > MAX_TEXT) {
      Serial.printf("INPUT INVALID: text must be 1..%u characters\n", MAX_TEXT);
      return;
    }
    for (size_t i = 0; i < text.length(); i++) {
      if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126) {
        Serial.println("INPUT INVALID: non-printable character");
        return;
      }
    }
    sendPeer('T', hexEncode(text), text, 0, 0, 0, 0);
    return;
  }

  if (upper.startsWith("DATA,")) {
    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    if (p4 < 0 || line.indexOf(',', p4 + 1) >= 0) {
      Serial.println("INPUT INVALID: expected DATA,<c1>,<c2>,<c3>,<c4>");
      return;
    }
    long v[4];
    int idx[5] = {p1, p2, p3, p4, (int)line.length()};
    for (uint8_t i = 0; i < 4; i++) {
      String t = line.substring(idx[i] + 1, idx[i + 1]);
      t.trim();
      for (size_t k = 0; k < t.length(); k++) {
        if (!isDigit(t[k])) {
          Serial.printf("INPUT INVALID: channel %u not numeric\n", i + 1);
          return;
        }
      }
      v[i] = t.toInt();
      if (v[i] < 0 || v[i] > 4095) {
        Serial.printf("INPUT INVALID: channel %u out of 0..4095\n", i + 1);
        return;
      }
    }
    String payload = String(v[0]) + ";" + String(v[1]) + ";" +
                     String(v[2]) + ";" + String(v[3]);
    sendPeer('D', payload, "", v[0], v[1], v[2], v[3]);
    return;
  }

  // Any other line is a text message.
  if (line.length() > MAX_TEXT) {
    Serial.printf("INPUT INVALID: text must be <= %u characters\n", MAX_TEXT);
    return;
  }
  for (size_t i = 0; i < line.length(); i++) {
    if ((uint8_t)line[i] < 32 || (uint8_t)line[i] > 126) {
      Serial.println("INPUT INVALID: non-printable character");
      return;
    }
  }
  sendPeer('T', hexEncode(line), line, 0, 0, 0, 0);
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  pinMode(LORA_DIO0, INPUT);

  uint64_t mac = ESP.getEfuseMac();
  char id[5];
  snprintf(id, sizeof(id), "%02X%02X",
           (uint8_t)(mac >> 8), (uint8_t)(mac & 0xFF));
  deviceId = String(id);

  prefs.begin("peer", false);
  sequenceNumber = prefs.getULong("seq", 0);

  Serial.println();
  Serial.println("========== LORA PEER ==========");
  Serial.printf("DEVICE ID: %s\n", deviceId.c_str());
  Serial.printf("SEQUENCE: %lu\n", (unsigned long)sequenceNumber);

  radioReady = initRadio("boot");
  if (!radioReady) {
    Serial.println("RADIO=FAIL - retrying");
  }
  printHelp();
  Serial.printf("PEERREADY,id=%s,radio=%s,freq=%lu,sf=%u,sync=0x%02X,seq=%lu\n",
                deviceId.c_str(), radioReady ? "OK" : "FAIL",
                (unsigned long)readFrequency(), LORA_SF, LORA_SYNC_WORD,
                (unsigned long)sequenceNumber);
  Serial.println("ENTER MESSAGE OR COMMAND:");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputLine.length() > 0) inputReady = true;
    } else if (inputLine.length() < 160) {
      inputLine += c;
    }
  }

  if (inputReady) {
    String line = inputLine;
    inputLine = "";
    inputReady = false;
    handleLine(line);
  }

  serviceRadio();

  if (!radioReady) {
    static unsigned long lastInit = 0;
    if (millis() - lastInit >= 3000) {
      lastInit = millis();
      radioReady = initRadio("recovery");
      if (radioReady) {
        Serial.println("RADIO=OK (recovered)");
        Serial.printf("PEERREADY,id=%s,radio=OK,freq=%lu,sf=%u,sync=0x%02X,seq=%lu\n",
                      deviceId.c_str(), (unsigned long)readFrequency(), LORA_SF,
                      LORA_SYNC_WORD, (unsigned long)sequenceNumber);
      }
    }
  } else {
    static unsigned long lastHealth = 0;
    if (millis() - lastHealth >= RADIO_HEALTH_MS) {
      lastHealth = millis();
      uint8_t v = regRead(REG_VERSION);
      uint8_t op = regRead(REG_OP_MODE);
      if (v != 0x12 || !(op & 0x80)) {
        Serial.printf("RADIO HEALTH FAIL version=0x%02X opmode=0x%02X\n", v, op);
        radioReady = false;
      }
    }
  }
}
