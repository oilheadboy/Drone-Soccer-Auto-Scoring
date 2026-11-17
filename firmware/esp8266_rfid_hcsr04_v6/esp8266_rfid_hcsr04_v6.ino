#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <string.h>

// =====================================
// 1. ESP8266 -> PCB 七段顯示器
// =====================================
const uint8_t DATA_PIN  = D5;
const uint8_t CLOCK_PIN = D6;
const uint8_t LATCH_PIN = D0;

// =====================================
// 2. YPD-R200 RFID 接腳
// =====================================
// SoftwareSerial(接收RX, 發送TX)
const uint8_t RFID_RX_PIN = D7;  // 接 YPD-R200 TXD
const uint8_t RFID_TX_PIN = D2;  // 接 YPD-R200 RXD

SoftwareSerial rfid(RFID_RX_PIN, RFID_TX_PIN);

// HC-SR04: D8 must remain LOW at boot; ECHO must be 3.3V-safe.
const uint8_t TRIG_PIN = D8;
const uint8_t ECHO_PIN = D1;
const float DETECT_CM = 35.0f;
const float CLEAR_CM = 40.0f;
const unsigned long US_PERIOD_MS = 65;
const unsigned long US_TIMEOUT_US = 30000;
const unsigned long SCAN_WINDOW_MS = 1500;
const unsigned long CLEAR_STABLE_MS = 200;

volatile uint8_t echoState = 0; // 0 idle, 1 rise, 2 fall, 3 ready
volatile uint32_t echoRise = 0, echoWidth = 0;
uint32_t pingStart = 0;
unsigned long lastPing = 0;
bool scanActive = false;
unsigned long scanStarted = 0;
bool clearKnown = false;
unsigned long clearSince = 0;
bool scoredEvent = false;
unsigned long scoredAt = 0;
float lastDistance = -1.0f;
bool freshDistance = false;

void ICACHE_RAM_ATTR echoISR() {
  bool high = digitalRead(ECHO_PIN);
  uint32_t t = micros();
  if (high && echoState == 1) {
    echoRise = t;
    echoState = 2;
  } else if (!high && echoState == 2) {
    echoWidth = t - echoRise;
    echoState = 3;
  }
}

// =====================================
// Wi-Fi / WSL TCP 伺服器（原本測試程式）
// =====================================
#include "config.h"
const uint16_t serverPort = 9090;
const unsigned long WIFI_RETRY_MS = 10000;
unsigned long lastWiFiRetry = 0;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Wi-Fi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nESP8266 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi not connected; RFID continues offline");
  }
  lastWiFiRetry = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiRetry < WIFI_RETRY_MS) return;
  lastWiFiRetry = millis();
  Serial.println("Retrying Wi-Fi...");
  WiFi.reconnect();
}

// 每次確認進球後，送一行文字到 WSL C++ TCP server。
// 注意：這不是 HTTP；server 需要依照 GOAL|分數|EPC 格式解析。
bool sendGoalToWSL(const char* epc, uint8_t newScore) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("GOAL NOT SENT: Wi-Fi offline");
    return false;
  }

  WiFiClient client;
  if (!client.connect(serverIP, serverPort)) {
    Serial.println("GOAL NOT SENT: WSL TCP connect failed");
    return false;
  }

  String message = "GOAL|" + String(newScore) + "|" + String(epc);
  size_t written = client.println(message);
  client.flush();
  client.stop();

  if (written != message.length() + 2) {
    Serial.println("GOAL SEND FAILED / INCOMPLETE");
    return false;
  }

  Serial.print("GOAL sent to WSL: ");
  Serial.println(message);
  return true;
}

// =====================================
// 3. RFID 與防重複計分參數
// =====================================
const unsigned long RFID_POLL_MS = 150;
const unsigned long CONFIRM_MS = 2000;
const unsigned long MIN_CONFIRM_GAP_MS = 120;

// 成功計分後至少等待 3 秒，且超音波確認清空
const unsigned long AUTO_REARM_MS = 3000;

const uint8_t REQUIRED_READS = 1; // 快速穿越測試；可改為 2 或 3
const uint16_t MAX_FRAME = 96;

// =====================================
// 4. 系統狀態
// =====================================
uint8_t score = 0;

// true  = 可以計分
// false = 計分完成，等待 TAG 離開
bool scoringArmed = false; // 開機先確認球門清空

// EPC 確認紀錄
char candidateEPC[65] = "";

uint8_t confirmCount = 0;
unsigned long firstSeen = 0;
unsigned long lastConfirmedAt = 0;

// 最近一次有效 RFID 讀取
unsigned long lastValidSeen = 0;
bool hasSeenTag = false;

// 封包統計
unsigned long validFrameCount = 0;
unsigned long invalidFrameCount = 0;

unsigned long lastPoll = 0;

// =====================================
// 5. RFID 接收緩衝區
// =====================================
uint8_t rxFrame[MAX_FRAME];

uint16_t rxIndex = 0;
uint16_t expectedLength = 0;

unsigned long lastByteTime = 0;

// =====================================
// 6. 更新 4 吋七段顯示器
// =====================================
void showScore() {
  byte tens = score / 10;
  byte ones = score % 10;

  // 高 4 bit = 十位數，低 4 bit = 個位數
  byte data = (tens << 4) | ones;

  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, data);
  digitalWrite(LATCH_PIN, HIGH);
}

// =====================================
// 7. 清除 EPC 確認紀錄
// =====================================
void clearCandidate() {
  candidateEPC[0] = '\0';
  confirmCount = 0;
  firstSeen = 0;
  lastConfirmedAt = 0;
}

// =====================================
// 8. 計分 +1
// =====================================
void addScore(const char *epc) {
  if (score < 99) {
    score++;
    showScore();

    Serial.print("SCORE = ");
    Serial.println(score);

    // RFID 確認成功才送往 TCP server；保留原本的本地計分模式。
    sendGoalToWSL(epc, score);
  } else {
    Serial.println("SCORE LIMIT = 99");
  }

  Serial.print("EPC = ");
  Serial.println(epc);
}

// =====================================
// 9. 發送 RFID 單次盤點指令
// =====================================
void requestRFID() {
  const uint8_t cmd[] = {
    0xAA, 0x00, 0x22,
    0x00, 0x00, 0x22, 0xDD
  };

  rfid.write(cmd, sizeof(cmd));
}

// =====================================
// 10. 處理有效 EPC
// =====================================
void processEPC(const char *epc) {
  unsigned long now = millis();
  // Ignore all EPC reports outside a triggered scan window.
  if (!scanActive || !scoringArmed || now - scanStarted >= SCAN_WINDOW_MS) return;

  lastValidSeen = now;
  hasSeenTag = true;
  // Optional: insert an allowed-EPC check here if only striker tags may score.
  if (confirmCount == 0 || strcmp(candidateEPC, epc) != 0 ||
      now - firstSeen > CONFIRM_MS) {
    strncpy(candidateEPC, epc, 64);
    candidateEPC[64] = '\0';
    confirmCount = 1;
    firstSeen = now;
    lastConfirmedAt = now;
    Serial.print("FIRST READ: ");
    Serial.println(candidateEPC);
  } else if (now - lastConfirmedAt >= MIN_CONFIRM_GAP_MS) {
    lastConfirmedAt = now;
    if (confirmCount < REQUIRED_READS) confirmCount++;
  }

  if (confirmCount >= REQUIRED_READS) {
    scanActive = false;
    scoringArmed = false;
    scoredEvent = true;
    scoredAt = now;
    clearKnown = false; // require a fresh clear reading after scoring
    Serial.println("RFID CONFIRMED - SCAN OFF");
    addScore(epc);
    clearCandidate();
  }
}

// =====================================
// 11. 解析 RFID 回傳封包
// =====================================
void processFrame(const uint8_t *f,
                  uint16_t total) {

  if (total < 7 ||
      f[0] != 0xAA ||
      f[total - 1] != 0xDD) {
    invalidFrameCount++;
    return;
  }

  uint16_t len =
    ((uint16_t)f[3] << 8) | f[4];

  if (len + 7 != total) {
    invalidFrameCount++;
    return;
  }

  // Checksum：Type 到最後一個 payload byte 的累加和低 8 bit
  uint8_t checksum = 0;

  for (uint16_t i = 1; i < 5 + len; i++) {
    checksum += f[i];
  }

  if (checksum != f[5 + len]) {
    invalidFrameCount++;
    return;
  }

  // 僅接受 EPC 通知封包
  if (f[1] != 0x02 || f[2] != 0x22) {
    return;
  }

  // RSSI(1) + PC(2) + EPC(N) + CRC(2)
  if (len < 7) {
    invalidFrameCount++;
    return;
  }

  uint16_t epcBytes = len - 5;

  if (epcBytes == 0 || epcBytes > 32) {
    invalidFrameCount++;
    return;
  }

  uint16_t pc =
    ((uint16_t)f[6] << 8) | f[7];

  uint16_t expectedEpcBytes =
    ((pc >> 11) & 0x1F) * 2;

  if (expectedEpcBytes != epcBytes) {
    invalidFrameCount++;
    return;
  }

  char epc[65];

  const char hexChars[] =
    "0123456789ABCDEF";

  for (uint16_t i = 0; i < epcBytes; i++) {
    uint8_t value = f[8 + i];

    epc[i * 2] =
      hexChars[value >> 4];

    epc[i * 2 + 1] =
      hexChars[value & 0x0F];
  }

  epc[epcBytes * 2] = '\0';

  validFrameCount++;

  processEPC(epc);
}

// =====================================
// 12. RFID UART 接收
// =====================================
void readRFID() {
  while (rfid.available() > 0) {

    uint8_t b = rfid.read();
    unsigned long now = millis();

    // 不完整封包超時
    if (rxIndex > 0 &&
        now - lastByteTime > 100) {

      rxIndex = 0;
      expectedLength = 0;
      invalidFrameCount++;
    }

    lastByteTime = now;

    // 尋找 AA 封包開頭
    if (rxIndex == 0) {
      if (b != 0xAA) continue;
      expectedLength = 0;
    }

    if (rxIndex >= MAX_FRAME) {
      rxIndex = 0;
      expectedLength = 0;
      invalidFrameCount++;
      continue;
    }

    rxFrame[rxIndex++] = b;

    // 讀取 Payload 長度
    if (rxIndex == 5) {

      uint16_t payloadLength =
        ((uint16_t)rxFrame[3] << 8) |
        rxFrame[4];

      uint16_t total = payloadLength + 7;

      if (total > MAX_FRAME || total < 7) {
        rxIndex = 0;
        expectedLength = 0;
        invalidFrameCount++;
        continue;
      }

      expectedLength = total;
    }

    // 收到完整封包
    if (expectedLength > 0 &&
        rxIndex == expectedLength) {

      processFrame(rxFrame, rxIndex);

      rxIndex = 0;
      expectedLength = 0;
    }
  }
}

// =====================================
// 13. 自動解鎖
// =====================================
// Start a scan only after a new object enters an armed, previously clear gate.
void startRFIDScan(unsigned long now) {
  clearCandidate();
  while (rfid.available() > 0) rfid.read();
  rxIndex = 0;
  expectedLength = 0;
  scanStarted = now;
  scanActive = true;
  scoredEvent = false;
  Serial.println("US DETECTED - RFID SCAN ON");
  requestRFID(); // first inventory immediately
  lastPoll = millis();
}

void updateUltrasonic() {
  unsigned long now = millis();
  uint32_t width = 0;
  bool completed = false;
  bool timeout = false;
  noInterrupts();
  if (echoState == 3) {
    width = echoWidth;
    echoState = 0;
    completed = true;
  } else if (echoState != 0 && (uint32_t)(micros() - pingStart) >= US_TIMEOUT_US) {
    echoState = 0;
    timeout = true;
  }
  bool idle = echoState == 0;
  interrupts();

  if (completed || timeout) {
    float cm = completed ? width * 0.0343f / 2.0f : -1.0f;
    freshDistance = completed && cm >= 2.0f && cm <= 400.0f;
    lastDistance = freshDistance ? cm : -1.0f;
    if (freshDistance && cm > CLEAR_CM) {
      if (!clearKnown) clearSince = now;
      clearKnown = true;
    } else {
      // No echo is unknown, not proof of a clear gate.
      clearKnown = false;
      if (freshDistance && cm <= DETECT_CM && scoringArmed && !scanActive) {
        startRFIDScan(now);
      }
    }
  }

  if (idle && now - lastPing >= US_PERIOD_MS) {
    lastPing = now;
    noInterrupts();
    pingStart = micros();
    echoState = 1;
    interrupts();
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
  }
}

bool canRearm(unsigned long now) {
  return !scanActive && clearKnown && now - clearSince >= CLEAR_STABLE_MS &&
         (!scoredEvent || now - scoredAt >= AUTO_REARM_MS);
}

void handleAutoRearm() {
  unsigned long now = millis();
  if (scanActive && now - scanStarted >= SCAN_WINDOW_MS) {
    scanActive = false;
    scoringArmed = false;
    clearCandidate();
    Serial.println("RFID SCAN TIMEOUT - NO SCORE");
  }
  if (!scoringArmed && canRearm(now)) {
    clearCandidate();
    scoringArmed = true;
    scoredEvent = false;
    Serial.println("READY FOR NEXT OBJECT - RFID IDLE");
  }
}

// =====================================
// 14. USB 序列監控備用指令
// =====================================
void handleSerialCommands() {
  while (Serial.available() > 0) {
    char command = Serial.read();

    // R = 手動重新解鎖
    if (command == 'R' || command == 'r') {

      if (scoringArmed && !scanActive) {
        Serial.println("ALREADY ARMED");
        continue;
      }
      if (!canRearm(millis())) {
        Serial.println("REARM DENIED: scanning, gate not clear, or cooldown");
        continue;
      }
      clearCandidate();
      scoringArmed = true;
      scoredEvent = false;

      Serial.println("MANUAL REARMED");
    }

    // C = 分數清零，不改變鎖定狀態
    else if (command == 'C' || command == 'c') {
      score = 0;
      showScore();

      Serial.println("SCORE RESET TO 00");
    }

    // S = 系統狀態
    else if (command == 'S' || command == 's') {

      Serial.print("SCORE = ");
      Serial.println(score);

      Serial.print("ARMED = ");
      Serial.println(
        scoringArmed ? "YES" : "NO"
      );

      Serial.print("RFID SCANNING = ");
      Serial.println(scanActive ? "YES" : "NO");
      Serial.print("DISTANCE CM = ");
      if (freshDistance) Serial.println(lastDistance, 2);
      else Serial.println("UNKNOWN");
      Serial.print("VALID FRAMES = ");
      Serial.println(validFrameCount);

      Serial.print("INVALID FRAMES = ");
      Serial.println(invalidFrameCount);

      if (hasSeenTag) {
        Serial.print("LAST TAG SEEN ");
        Serial.print(millis() - lastValidSeen);
        Serial.println(" ms ago");
      } else {
        Serial.println("NO TAG SEEN YET");
      }
    }
  }
}

// =====================================
// 15. 初始化
// =====================================
void setup() {

  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  digitalWrite(LATCH_PIN, HIGH);

  // HC-SR04
  digitalWrite(TRIG_PIN, LOW);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(ECHO_PIN), echoISR, CHANGE);

  // USB 序列監控
  Serial.begin(115200);

  // YPD-R200 RFID
  rfid.begin(115200);

  score = 0;
  scoringArmed = false;

  hasSeenTag = false;
  lastValidSeen = 0;

  clearCandidate();
  showScore();

  delay(1000);

  Serial.println();
  Serial.println("RFID AUTO SCORE V6 - ULTRASONIC TRIGGER READY");
  Serial.println("AUTO REARM = 3000 ms");
  Serial.println("Commands: R / C / S");

  // 連接 Wi-Fi；逾時不會阻止 RFID 本地計分。
  connectWiFi();
  lastPoll = millis();
}

// =====================================
// 16. 主程式
// =====================================
void loop() {

  // 優先處理 RFID 接收
  readRFID();

  updateUltrasonic();
  handleAutoRearm();

  // 備用序列監控指令
  handleSerialCommands();

  // 背景維持 Wi-Fi（每 10 秒檢查一次重新連線）
  maintainWiFi();

  unsigned long now = millis();

  // EPC 確認時間窗超時
  if (scoringArmed &&
      confirmCount > 0 &&
      now - firstSeen > CONFIRM_MS) {

    clearCandidate();
  }

  // 定期發送 RFID 盤點指令
  if (scanActive && now - scanStarted < SCAN_WINDOW_MS &&
      now - lastPoll >= RFID_POLL_MS) {

    requestRFID();
    lastPoll = now;
  }

  yield();
}