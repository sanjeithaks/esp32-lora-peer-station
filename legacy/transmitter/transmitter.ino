/*
 * transmitter.ino - LoRa manual telemetry + message transmitter with ACK
 * ----------------------------------------------------------------------
 * ROLE      : TRANSMITTER
 * USB PORT  : COM4 (the dashboard also verifies this by firmware role)
 * HARDWARE  : ESP32 + RA-02 (SX1278) 433 MHz, no joysticks, no ADC input.
 *
 * Serial commands (115200 baud, newline terminated):
 *   SEND,1500,1000,2000,1500   four channels, each 0..4095  -> telemetry
 *   HELLO RECEIVER             any other text (1..32 chars)  -> message
 *   STATUS                     diagnostics
 *   HELP                       command list
 *
 * Two-way protocol:
 *   TX -> RX : DAT,<seq>,<T|M>,<c1>,<c2>,<c3>,<c4>,<hex-text>
 *   RX -> TX : ACK,<seq>
 * A transmission is only reported as DELIVERY CONFIRMED when the ACK for that
 * exact sequence number arrives from the receiver.
 *
 * Wiring (both boards): SCK=18 MISO=19 MOSI=23 NSS=5 RST=14 DIO0=26, 3.3 V.
 */

#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>

#define FIRMWARE_ROLE "TRANSMITTER"
#define FIRMWARE_PORT "COM4"

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

#define TX_DONE_TIMEOUT_MS   1200
#define ACK_TIMEOUT_MS       1800
#define MAX_ATTEMPTS         3
#define MAX_TEXT_LEN         32
#define INIT_ATTEMPTS        3

#define REG_IRQ_FLAGS    0x12
#define REG_OP_MODE      0x01
#define REG_VERSION      0x42
#define REG_FRF_MSB      0x06
#define REG_FRF_MID      0x07
#define REG_FRF_LSB      0x08

Preferences prefs;
bool radioReady = false;
uint32_t sequenceNumber = 0;

uint32_t txAttempts = 0;
uint32_t txOkCount = 0;
uint32_t txFailCount = 0;
uint32_t ackOkCount = 0;
uint32_t ackTimeoutCount = 0;
unsigned long lastTxMs = 0;
unsigned long lastAckMs = 0;

String inputLine;
bool inputReady = false;
bool busy = false;

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
    LoRa.setTxPower(LORA_TX_POWER, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.idle();

    uint32_t f = readFrequency();
    if (f < 432000000UL || f > 434000000UL) {
      Serial.printf("-> FAIL tuning %lu Hz (expected 433000000)\n",
                    (unsigned long)f);
      delay(200);
      continue;
    }
    Serial.println("-> OK (433 MHz, sync 0x12)");
    return true;
  }
  return false;
}

// ------------------------------------------------------------ output
String hexEncode(const String &s) {
  static const char *digits = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    out += digits[c >> 4];
    out += digits[c & 0x0F];
  }
  return out;
}

String hexDecode(const String &s) {
  String out;
  for (size_t i = 0; i + 1 < s.length(); i += 2) {
    char buf[3] = {s[i], s[i + 1], 0};
    out += (char)strtol(buf, nullptr, 16);
  }
  return out;
}

void printHeader(const char *title) {
  Serial.println();
  Serial.println("========================================");
  Serial.printf("         %s\n", title);
  Serial.println("========================================");
  Serial.println();
}

void printDiagnostics() {
  uint8_t version = regRead(REG_VERSION);
  uint8_t op = regRead(REG_OP_MODE);
  uint32_t f = readFrequency();
  bool ok = radioReady && version == 0x12 &&
            f > 432000000UL && f < 434000000UL;

  Serial.println("HARDWARE DIAGNOSTICS:");
  Serial.println("ESP32 STATUS: CONNECTED");
  Serial.println("SPI STATUS: CHECKED");
  Serial.printf("SX1278 VERSION: 0x%02X %s\n", version,
                version == 0x12 ? "OK" : "UNEXPECTED");
  Serial.printf("LORA STATUS: %s\n", radioReady ? "INITIALIZED" : "FAILED");
  Serial.printf("LORA OP MODE: 0x%02X\n", op);
  Serial.printf("LORA FREQUENCY: %lu Hz (%s)\n", (unsigned long)f,
                (f > 432000000UL && f < 434000000UL) ? "OK" : "INVALID");
  Serial.printf("LORA SYNC WORD: 0x%02X\n", regRead(0x39));
  Serial.printf("SPREADING FACTOR: SF%u\n", LORA_SF);
  uint8_t mc1 = regRead(0x1D);
  uint8_t mc2 = regRead(0x1E);
  Serial.printf("BANDWIDTH CODE: %u (7 = 125 kHz)\n", (mc1 >> 4) & 0x0F);
  Serial.printf("CODING RATE CODE: %u (1 = 4/5)\n", (mc1 >> 1) & 0x07);
  Serial.printf("HEADER MODE: %s\n", (mc1 & 1) ? "IMPLICIT" : "EXPLICIT");
  Serial.printf("PREAMBLE: %u symbols\n",
                ((uint16_t)regRead(0x20) << 8) | regRead(0x21));
  Serial.printf("CRC: %s\n", (mc2 & 0x04) ? "ON" : "OFF");
  Serial.printf("TRANSMISSIONS: %lu attempted, %lu local success, %lu failed\n",
                (unsigned long)txAttempts, (unsigned long)txOkCount,
                (unsigned long)txFailCount);
  Serial.printf("ACKNOWLEDGEMENTS: %lu received, %lu timed out\n",
                (unsigned long)ackOkCount, (unsigned long)ackTimeoutCount);
  Serial.printf("PACKET SEQUENCE: %lu\n", (unsigned long)sequenceNumber);
  if (!ok) {
    Serial.println("RECOMMENDATION: check RA-02 power (3.3 V), GND, SPI wiring, "
                   "RESET and antenna. Re-run STATUS after repair.");
  }
}

void printTxStatusLine(uint32_t seq, char type,
                       uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4,
                       const String &textHex, uint8_t attempts,
                       bool txOk, bool ackOk, int ackRssi, float ackSnr) {
  Serial.printf("TXSTAT,seq=%lu,type=%c,c1=%u,c2=%u,c3=%u,c4=%u,text=%s,"
                "attempts=%u,tx=%s,ack=%s,ackrssi=%d,acksnr=%.2f,"
                "txok=%lu,txfail=%lu,ackok=%lu,ackto=%lu,ms=%lu,"
                "role=TRANSMITTER,port=COM4\n",
                (unsigned long)seq, type, c1, c2, c3, c4,
                textHex.length() ? textHex.c_str() : "-",
                attempts, txOk ? "OK" : "FAIL",
                ackOk ? "OK" : "TIMEOUT", ackRssi, ackSnr,
                (unsigned long)txOkCount, (unsigned long)txFailCount,
                (unsigned long)ackOkCount, (unsigned long)ackTimeoutCount,
                millis());
}

// ------------------------------------------------------------ radio actions
bool transmitPayload(const String &payload) {
  if (!radioReady) return false;

  regWrite(REG_IRQ_FLAGS, 0xFF);
  if (!LoRa.beginPacket()) {
    Serial.println("TX: beginPacket() failed (radio busy)");
    return false;
  }
  LoRa.print(payload);
  LoRa.endPacket(true);          // async; we wait for TxDone with a timeout

  unsigned long start = millis();
  while (millis() - start < TX_DONE_TIMEOUT_MS) {
    if (regRead(REG_IRQ_FLAGS) & 0x08) {
      regWrite(REG_IRQ_FLAGS, 0xFF);
      LoRa.idle();
      return true;
    }
    delay(2);
  }
  Serial.println("TX: TxDone timeout (SX1278 did not complete transmission)");
  regWrite(REG_IRQ_FLAGS, 0xFF);
  LoRa.idle();
  return false;
}

bool waitForAck(uint32_t expectedSeq, unsigned long timeoutMs,
                int &ackRssi, float &ackSnr) {
  LoRa.receive();
  unsigned long start = millis();
  unsigned long lastRearm = 0;

  while (millis() - start < timeoutMs) {
    int size = LoRa.parsePacket();
    if (size > 0) {
      String msg;
      msg.reserve(size);
      while (LoRa.available()) msg += (char)LoRa.read();
      ackRssi = LoRa.packetRssi();
      ackSnr = LoRa.packetSnr();
      LoRa.receive();

      if (msg == "ACK," + String(expectedSeq)) return true;
      // Any other packet is not our acknowledgement: keep waiting.
    } else if (millis() - lastRearm >= 200) {
      lastRearm = millis();
      uint8_t op = regRead(REG_OP_MODE);
      if ((op & 0x80) && ((op & 0x07) != 5)) {   // not RX_CONTINUOUS
        regWrite(REG_IRQ_FLAGS, 0xFF);
        LoRa.receive();
      }
    }
    delay(2);
  }
  return false;
}

// ------------------------------------------------------------ manual input
void handleTelemetry(const String &line) {
  // SEND,<c1>,<c2>,<c3>,<c4>
  int p1 = line.indexOf(',');
  int p2 = line.indexOf(',', p1 + 1);
  int p3 = line.indexOf(',', p2 + 1);
  int p4 = line.indexOf(',', p3 + 1);
  if (p4 < 0 || line.indexOf(',', p4 + 1) >= 0) {
    printHeader("LORA TRANSMITTER");
    Serial.println("MANUAL TELEMETRY INPUT:");
    Serial.println(line);
    Serial.println();
    Serial.println("INPUT VALIDATION: FAILED");
    Serial.println("REASON: expected SEND,<c1>,<c2>,<c3>,<c4> (exactly 4 values)");
    Serial.println("EXAMPLE: SEND,1500,1000,2000,1500");
    return;
  }

  long v[4];
  int idx[5] = {p1, p2, p3, p4, (int)line.length()};
  for (uint8_t i = 0; i < 4; i++) {
    String token = line.substring(idx[i] + 1, idx[i + 1]);
    token.trim();
    if (token.length() == 0 || token.length() > 5) {
      printHeader("LORA TRANSMITTER");
      Serial.printf("INPUT VALIDATION: FAILED\nREASON: channel %u is not a number\n", i + 1);
      return;
    }
    for (size_t k = 0; k < token.length(); k++) {
      if (!isDigit(token[k])) {
        printHeader("LORA TRANSMITTER");
        Serial.printf("INPUT VALIDATION: FAILED\nREASON: channel %u is not numeric\n", i + 1);
        return;
      }
    }
    v[i] = token.toInt();
    if (v[i] < 0 || v[i] > 4095) {
      printHeader("LORA TRANSMITTER");
      Serial.printf("INPUT VALIDATION: FAILED\nREASON: channel %u = %ld out of range 0..4095\n",
                    i + 1, v[i]);
      return;
    }
  }

  sequenceNumber++;
  prefs.putULong("seq", sequenceNumber);

  String payload = "DAT," + String(sequenceNumber) + ",T," +
                   String(v[0]) + "," + String(v[1]) + "," +
                   String(v[2]) + "," + String(v[3]) + ",-";

  Serial.printf("TXCMD,seq=%lu,type=T,c1=%ld,c2=%ld,c3=%ld,c4=%ld,cmd=%s,ms=%lu\n",
                (unsigned long)sequenceNumber, v[0], v[1], v[2], v[3],
                hexEncode(line).c_str(), millis());
  Serial.printf("TXSTAGE,seq=%lu,stage=STARTED,ms=%lu\n",
                (unsigned long)sequenceNumber, millis());

  printHeader("LORA TRANSMITTER");
  Serial.println("DEVICE: ESP32 TRANSMITTER");
  Serial.println("COM PORT: COM4");
  Serial.printf("SYSTEM STATUS: %s\n", radioReady ? "INITIALIZED" : "RADIO NOT READY");
  printDiagnostics();
  Serial.println();
  Serial.println("MANUAL TELEMETRY INPUT:");
  Serial.println(line);
  Serial.println("INPUT VALIDATION: SUCCESS");
  Serial.println();
  Serial.println("TRANSMISSION INFORMATION:");
  Serial.printf("PACKET SEQUENCE: %03lu\n", (unsigned long)sequenceNumber);
  Serial.println("PACKET TYPE: TELEMETRY");
  Serial.printf("CHANNEL 1: %ld\nCHANNEL 2: %ld\nCHANNEL 3: %ld\nCHANNEL 4: %ld\n",
                v[0], v[1], v[2], v[3]);
  Serial.println();

  if (!radioReady) {
    Serial.println("LORA TRANSMISSION: FAILED (radio not initialized)");
    Serial.println("RECEIVER ACKNOWLEDGEMENT: NOT POSSIBLE");
    Serial.println("FINAL STATUS: DELIVERY NOT CONFIRMED");
    txFailCount++;
    printTxStatusLine(sequenceNumber, 'T', v[0], v[1], v[2], v[3], "-",
                      0, false, false, 0, 0.0f);
    return;
  }

  bool ackOk = false;
  bool anyTxOk = false;
  uint8_t attempts = 0;
  int ackRssi = 0;
  float ackSnr = 0.0f;

  for (uint8_t attempt = 1; attempt <= MAX_ATTEMPTS && !ackOk; attempt++) {
    attempts = attempt;
    txAttempts++;
    Serial.printf("LORA TRANSMISSION: ATTEMPT %u/%u ... ", attempt, MAX_ATTEMPTS);
    bool txOk = transmitPayload(payload);
    Serial.println(txOk ? "SUCCESS" : "FAILED");
    Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, txOk ? "TX_COMPLETE" : "TX_FAILED",
                  attempt, millis());
    if (txOk) {
      txOkCount++;
      anyTxOk = true;
      lastTxMs = millis();
    } else {
      txFailCount++;
      if (!radioReady) break;
    }

    Serial.println("WAITING FOR ACKNOWLEDGEMENT...");
    Serial.printf("TXSTAGE,seq=%lu,stage=WAITING_ACK,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, attempt, millis());
    ackOk = waitForAck(sequenceNumber, ACK_TIMEOUT_MS, ackRssi, ackSnr);
    if (ackOk) {
      ackOkCount++;
      lastAckMs = millis();
      Serial.printf("RECEIVER ACKNOWLEDGEMENT: RECEIVED (attempt %u, "
                    "RSSI %d dBm, SNR %.2f dB)\n", attempt, ackRssi, ackSnr);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_RECEIVED,attempt=%u,rssi=%d,"
                    "snr=%.2f,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, ackRssi, ackSnr, millis());
    } else {
      ackTimeoutCount++;
      Serial.printf("RECEIVER ACKNOWLEDGEMENT: TIMEOUT (attempt %u)\n", attempt);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_TIMEOUT,attempt=%u,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, millis());
    }
  }

  Serial.println();
  if (ackOk) {
    Serial.println("FINAL STATUS: DELIVERY CONFIRMED");
    Serial.println("The receiver acknowledged this exact packet sequence.");
  } else if (anyTxOk) {
    Serial.println("FINAL STATUS: DELIVERY NOT CONFIRMED");
    Serial.println("Local transmission completed but no valid ACK was received.");
    Serial.println("RECOMMENDATION: check receiver power/USB, antennas, frequency, "
                   "sync word and the wireless range.");
  } else {
    Serial.println("FINAL STATUS: TRANSMISSION FAILED");
    Serial.println("RECOMMENDATION: check SX1278 power, SPI wiring and antenna; "
                   "run STATUS.");
  }
  Serial.println();
  Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempts=%u,tx=%s,ack=%s,ms=%lu\n",
                (unsigned long)sequenceNumber,
                ackOk ? "DELIVERY_CONFIRMED" :
                anyTxOk ? "DELIVERY_NOT_CONFIRMED" : "TRANSMISSION_FAILED",
                attempts, anyTxOk ? "OK" : "FAIL", ackOk ? "OK" : "TIMEOUT",
                millis());
  printTxStatusLine(sequenceNumber, 'T', v[0], v[1], v[2], v[3], "-",
                    attempts, anyTxOk, ackOk, ackRssi, ackSnr);
}

void handleMessage(const String &text) {
  if (text.length() == 0 || text.length() > MAX_TEXT_LEN) {
    printHeader("LORA TRANSMITTER");
    Serial.println("INPUT VALIDATION: FAILED");
    Serial.printf("REASON: message length must be 1..%u characters\n", MAX_TEXT_LEN);
    return;
  }
  for (size_t i = 0; i < text.length(); i++) {
    if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126) {
      printHeader("LORA TRANSMITTER");
      Serial.println("INPUT VALIDATION: FAILED");
      Serial.println("REASON: message contains non-printable characters");
      return;
    }
  }

  sequenceNumber++;
  prefs.putULong("seq", sequenceNumber);
  String hex = hexEncode(text);
  String payload = "DAT," + String(sequenceNumber) + ",M,0,0,0,0," + hex;

  Serial.printf("TXCMD,seq=%lu,type=M,c1=0,c2=0,c3=0,c4=0,cmd=%s,ms=%lu\n",
                (unsigned long)sequenceNumber, hexEncode(text).c_str(), millis());
  Serial.printf("TXSTAGE,seq=%lu,stage=STARTED,ms=%lu\n",
                (unsigned long)sequenceNumber, millis());

  printHeader("LORA TRANSMITTER");
  Serial.println("DEVICE: ESP32 TRANSMITTER");
  Serial.println("COM PORT: COM4");
  Serial.printf("SYSTEM STATUS: %s\n", radioReady ? "INITIALIZED" : "RADIO NOT READY");
  printDiagnostics();
  Serial.println();
  Serial.println("MANUAL MESSAGE INPUT:");
  Serial.println(text);
  Serial.println("INPUT VALIDATION: SUCCESS");
  Serial.println();
  Serial.println("TRANSMISSION INFORMATION:");
  Serial.printf("PACKET SEQUENCE: %03lu\n", (unsigned long)sequenceNumber);
  Serial.println("PACKET TYPE: MESSAGE");
  Serial.println();

  bool ackOk = false, anyTxOk = false;
  uint8_t attempts = 0;
  int ackRssi = 0;
  float ackSnr = 0.0f;

  for (uint8_t attempt = 1; attempt <= MAX_ATTEMPTS && !ackOk; attempt++) {
    attempts = attempt;
    txAttempts++;
    Serial.printf("LORA TRANSMISSION: ATTEMPT %u/%u ... ", attempt, MAX_ATTEMPTS);
    bool txOk = transmitPayload(payload);
    Serial.println(txOk ? "SUCCESS" : "FAILED");
    Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, txOk ? "TX_COMPLETE" : "TX_FAILED",
                  attempt, millis());
    if (txOk) { txOkCount++; anyTxOk = true; lastTxMs = millis(); }
    else { txFailCount++; if (!radioReady) break; }

    Serial.println("WAITING FOR ACKNOWLEDGEMENT...");
    Serial.printf("TXSTAGE,seq=%lu,stage=WAITING_ACK,attempt=%u,ms=%lu\n",
                  (unsigned long)sequenceNumber, attempt, millis());
    ackOk = waitForAck(sequenceNumber, ACK_TIMEOUT_MS, ackRssi, ackSnr);
    if (ackOk) {
      ackOkCount++; lastAckMs = millis();
      Serial.printf("RECEIVER ACKNOWLEDGEMENT: RECEIVED (attempt %u, RSSI %d dBm, SNR %.2f dB)\n",
                    attempt, ackRssi, ackSnr);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_RECEIVED,attempt=%u,rssi=%d,"
                    "snr=%.2f,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, ackRssi, ackSnr, millis());
    } else {
      ackTimeoutCount++;
      Serial.printf("RECEIVER ACKNOWLEDGEMENT: TIMEOUT (attempt %u)\n", attempt);
      Serial.printf("TXSTAGE,seq=%lu,stage=ACK_TIMEOUT,attempt=%u,ms=%lu\n",
                    (unsigned long)sequenceNumber, attempt, millis());
    }
  }

  Serial.println();
  if (ackOk) {
    Serial.println("FINAL STATUS: DELIVERY CONFIRMED");
    Serial.println("The receiver acknowledged this exact packet sequence.");
  } else if (anyTxOk) {
    Serial.println("FINAL STATUS: DELIVERY NOT CONFIRMED");
    Serial.println("Local transmission completed but no valid ACK was received.");
    Serial.println("RECOMMENDATION: check receiver power/USB, antennas, frequency, "
                   "sync word and range.");
  } else {
    Serial.println("FINAL STATUS: TRANSMISSION FAILED");
    Serial.println("RECOMMENDATION: check SX1278 power, SPI wiring and antenna.");
  }
  Serial.println();
  Serial.printf("TXSTAGE,seq=%lu,stage=%s,attempts=%u,tx=%s,ack=%s,ms=%lu\n",
                (unsigned long)sequenceNumber,
                ackOk ? "DELIVERY_CONFIRMED" :
                anyTxOk ? "DELIVERY_NOT_CONFIRMED" : "TRANSMISSION_FAILED",
                attempts, anyTxOk ? "OK" : "FAIL", ackOk ? "OK" : "TIMEOUT",
                millis());
  printTxStatusLine(sequenceNumber, 'M', 0, 0, 0, 0, hex,
                    attempts, anyTxOk, ackOk, ackRssi, ackSnr);
}

void printHelp() {
  printHeader("LORA TRANSMITTER");
  Serial.println("DEVICE: ESP32 TRANSMITTER");
  Serial.println("COM PORT: COM4");
  Serial.println();
  Serial.println("COMMANDS:");
  Serial.println("  SEND,<c1>,<c2>,<c3>,<c4>   telemetry (0..4095), e.g. SEND,1500,1000,2000,1500");
  Serial.println("  <text>                     message, 1..32 characters, e.g. HELLO RECEIVER");
  Serial.println("  STATUS                     full diagnostics");
  Serial.println("  HELP                       this list");
  Serial.println();
  Serial.println("Nothing is transmitted until you press Enter.");
  Serial.println("Acknowledgement is required before DELIVERY CONFIRMED is shown.");
}

// ------------------------------------------------------------ main
void setup() {
  Serial.begin(115200);
  delay(1500);

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);
  pinMode(LORA_DIO0, INPUT);

  prefs.begin("lora_tx", false);
  sequenceNumber = prefs.getULong("seq", 0);
  prefs.end();

  printHeader("LORA TRANSMITTER");
  Serial.println("DEVICE: ESP32 TRANSMITTER");
  Serial.println("COM PORT: COM4");
  Serial.println("ROLE=TRANSMITTER");
  Serial.println("PACKET FORMAT: DAT,<seq>,<T|M>,<c1>,<c2>,<c3>,<c4>,<hex>");
  Serial.println("ACK FORMAT: ACK,<seq>");
  Serial.println();

  radioReady = initRadio("boot");
  if (!radioReady) {
    Serial.println("SYSTEM STATUS: RADIO ERROR - retrying in background");
  } else {
    Serial.println("SYSTEM STATUS: INITIALIZED");
  }
  printDiagnostics();

  Serial.printf("TXREADY,role=TRANSMITTER,port=COM4,freq=%lu,sf=%u,sync=0x%02X,"
                "radio=%s,seq=%lu\n",
                (unsigned long)readFrequency(), LORA_SF, LORA_SYNC_WORD,
                radioReady ? "OK" : "FAIL", (unsigned long)sequenceNumber);
  printHelp();
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

  if (!inputReady) {
    if (!radioReady) {
      static unsigned long lastInit = 0;
      if (millis() - lastInit > 3000) {
        lastInit = millis();
        radioReady = initRadio("recovery");
        if (radioReady) Serial.println("SYSTEM STATUS: RECOVERED");
      }
    }
    return;
  }

  inputLine.trim();
  String line = inputLine;
  inputLine = "";
  inputReady = false;
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();
  busy = true;

  if (upper == "HELP" || upper == "?") {
    printHelp();
  } else if (upper == "STATUS") {
    printHeader("LORA TRANSMITTER");
    printDiagnostics();
    Serial.printf("LAST TX: %lu ms uptime\n", lastTxMs);
    Serial.printf("LAST ACK: %lu ms uptime\n", lastAckMs);
    Serial.printf("NAME: %s\n", FIRMWARE_ROLE);
  } else if (upper.startsWith("SEND,")) {
    handleTelemetry(line);
  } else {
    handleMessage(line);
  }

  busy = false;
  Serial.println("ENTER MESSAGE OR COMMAND:");
}
