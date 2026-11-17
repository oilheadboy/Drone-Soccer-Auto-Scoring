#include <Arduino.h>
#include <SoftwareSerial.h>
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

// =====================================
// 3. RFID 與防重複計分參數
// =====================================
const unsigned long RFID_POLL_MS = 300;
const unsigned long CONFIRM_MS = 2000;
const unsigned long MIN_CONFIRM_GAP_MS = 120;

// 連續 3 秒沒收到有效 EPC，自動解鎖
const unsigned long AUTO_REARM_MS = 3000;

// 手動 R 解鎖安全等待時間
const unsigned long MANUAL_REARM_MS = 3000;

const uint8_t REQUIRED_READS = 3;
const uint16_t MAX_FRAME = 96;

// =====================================
// 4. 系統狀態
// =====================================
uint8_t score = 0;

// true  = 可以計分
// false = 計分完成，等待 TAG 離開
bool scoringArmed = true;

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

  // 不論是否鎖定，都更新讀取時間
  lastValidSeen = now;
  hasSeenTag = true;

  // 已加分，保持鎖定
  if (!scoringArmed) {
    return;
  }

  // 第一次讀到 EPC，或換成不同 EPC
  if (confirmCount == 0 ||
      strcmp(candidateEPC, epc) != 0) {

    strncpy(candidateEPC, epc, 64);
    candidateEPC[64] = '\0';

    confirmCount = 1;
    firstSeen = now;
    lastConfirmedAt = now;

    Serial.print("FIRST READ: ");
    Serial.println(candidateEPC);

    return;
  }

  // 超過確認時間窗，重新開始
  if (now - firstSeen > CONFIRM_MS) {
    confirmCount = 1;
    firstSeen = now;
    lastConfirmedAt = now;
    return;
  }

  // 避免短時間內重複封包
  if (now - lastConfirmedAt < MIN_CONFIRM_GAP_MS) {
    return;
  }

  lastConfirmedAt = now;

  if (confirmCount < REQUIRED_READS) {
    confirmCount++;
  }

  // 達到 3 次有效確認
  if (confirmCount >= REQUIRED_READS) {

    // 先鎖定，防止重複加分
    scoringArmed = false;

    addScore(epc);

    clearCandidate();

    Serial.println("TAG LOCKED");
    Serial.println("WAITING FOR TAG EXIT...");
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
void handleAutoRearm() {

  // 沒有讀過 TAG，不需解鎖
  if (!hasSeenTag) return;

  // 已經是待命狀態
  if (scoringArmed) return;

  unsigned long now = millis();

  // 5 秒沒有收到有效 EPC
  if (now - lastValidSeen >= AUTO_REARM_MS) {

    clearCandidate();

    // 允許下一次加分
    scoringArmed = true;

    Serial.println("AUTO REARMED");
    Serial.println("READY FOR NEXT TAG");
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

      if (scoringArmed) {
        Serial.println("ALREADY ARMED");
        continue;
      }

      unsigned long now = millis();

      if (hasSeenTag &&
          now - lastValidSeen < MANUAL_REARM_MS) {

        Serial.println("REARM DENIED");
        Serial.println("TAG DETECTED RECENTLY");
        continue;
      }

      clearCandidate();
      scoringArmed = true;

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

  // USB 序列監控
  Serial.begin(115200);

  // YPD-R200 RFID
  rfid.begin(115200);

  score = 0;
  scoringArmed = true;

  hasSeenTag = false;
  lastValidSeen = 0;

  clearCandidate();
  showScore();

  delay(1000);

  Serial.println();
  Serial.println("RFID AUTO SCORE V4 READY");
  Serial.println("AUTO REARM = 3000 ms");
  Serial.println("Commands: R / C / S");

  lastPoll = millis();
}

// =====================================
// 16. 主程式
// =====================================
void loop() {

  // 優先處理 RFID 接收
  readRFID();

  // 5 秒未讀到 TAG，自動解鎖
  handleAutoRearm();

  // 備用序列監控指令
  handleSerialCommands();

  unsigned long now = millis();

  // EPC 確認時間窗超時
  if (scoringArmed &&
      confirmCount > 0 &&
      now - firstSeen > CONFIRM_MS) {

    clearCandidate();
  }

  // 定期發送 RFID 盤點指令
  if (now - lastPoll >= RFID_POLL_MS) {

    requestRFID();
    lastPoll = now;
  }

  yield();
}
