# Drone-Soccer-Auto-Scoring
# ESP8266 + YPD-R200 RFID 自動計分系統

## 專案介紹

這個程式是無人機足球自動計分系統的一部分，主要使用 ESP8266 搭配 YPD-R200 RFID 讀寫器，來判斷無人機是否通過球門，並將分數顯示在 PCB 連接的 4 吋七段顯示器上。

一開始主要是測試 ESP8266 能不能正常控制七段顯示器，確認顯示功能沒問題後，再加入 RFID 讀取和自動計分功能。

目前的程式除了可以自動加分，也加入了重複讀取確認、計分鎖定以及自動解鎖功能，避免 RFID 一直讀到同一個 TAG，就造成分數不停增加的問題。

### 目前完成的功能

- ESP8266 透過 UART 與 YPD-R200 RFID 讀寫器通訊。
- 每隔 300 ms 發送一次 RFID 讀取指令。
- 檢查 RFID 回傳的資料是否完整、正確。
- 讀取並解析 RFID TAG 的 EPC 編碼。
- 同一個 EPC 必須在 2 秒內成功確認 3 次，才會加分。
- 加分後會先鎖定，避免重複計分。
- 連續 3 秒沒讀到有效 EPC，會自動解除鎖定。
- 分數可以顯示 00～99。
- 透過 74HC595 控制 PCB 上的七段顯示器。
- 可以使用 Serial Monitor 的 R、C、S 指令進行測試。

---

## 1. GitHub 專案目錄

目前將程式分成兩個部分，主要是希望保留之前的測試紀錄，之後如果遇到問題，也比較容易回頭檢查。

```text
Drone-Soccer-Auto-Scoring/
└── firmware/
    ├── esp8266_ir_test/
    │   ├── README.md
    │   └── esp8266_ir_test.ino
    │
    └── esp8266_rfid_score/
        ├── README.md
        └── esp8266_rfid_score.ino
```

`esp8266_ir_test` 是之前測試 PCB 七段顯示器使用的程式。

`esp8266_rfid_score` 則是這次整合 YPD-R200 RFID 讀寫器的自動計分程式。

兩個程式先分開保存，避免後續修改 RFID 功能時，影響到原本已經測試成功的顯示功能。

## 2. 系統架構

整個系統主要分成 RFID 讀取、ESP8266 資料處理，以及七段顯示器三個部分。

當 RFID TAG 進入讀寫器的感應範圍後，YPD-R200 會將讀取到的資料透過 UART 傳送給 ESP8266。

ESP8266 收到資料後，會先確認封包有沒有問題，再取得 EPC 編碼，接著交給計分程式判斷。

如果符合加分條件，就會更新分數，並透過 74HC595 將資料送到 PCB 上的七段顯示電路。

系統架構圖會另外使用 draw.io 製作，方便之後放到專題報告中。

## 3. ESP8266 接腳設定

### PCB 七段顯示器

目前使用 ESP8266 的 D5、D6、D0，分別控制 74HC595 的資料、時脈與鎖存訊號。

| ESP8266 | GPIO | PCB 訊號 | 功能 |
|---|---|---|---|
| D5 | GPIO14 | DATA / DA | 傳送資料 |
| D6 | GPIO12 | CLOCK / CLK | 時脈控制 |
| D0 | GPIO16 | LATCH | 更新輸出資料 |
| GND | - | GND | 共地 |

程式設定如下：

```cpp
const uint8_t DATA_PIN  = D5;
const uint8_t CLOCK_PIN = D6;
const uint8_t LATCH_PIN = D0;
```

這組接腳設定是延續之前七段顯示器測試的方式，主要是希望整合 RFID 後，不需要再大幅修改原本的 PCB 控制程式。

## 4. ESP8266 與 YPD-R200 接線

RFID 讀寫器是透過 UART 和 ESP8266 傳送資料。

UART 接線要注意 TX 和 RX 必須交叉連接，也就是一邊發送資料，另一邊負責接收。

| ESP8266 | YPD-R200 | 功能 |
|---|---|---|
| D7 / GPIO13 | TXD | 接收 RFID 資料 |
| D2 / GPIO4 | RXD | 發送 RFID 指令 |
| GND | GND | 共地 |
| 外部穩定 5V 電源 | VCC / 5V | RFID 供電 |

程式設定：

```cpp
const uint8_t RFID_RX_PIN = D7;
const uint8_t RFID_TX_PIN = D2;

SoftwareSerial rfid(RFID_RX_PIN, RFID_TX_PIN);
```

### 電壓注意事項

YPD-R200 的供電範圍約為 3.6～5.5V，目前規劃使用穩定的 5V 電源供電。

不過 ESP8266 的 GPIO 是 3.3V 邏輯，所以接線之前還是要確認 RFID 的 TXD 輸出電壓。

如果輸出是 5V，就需要先經過電平轉換或分壓，不能直接接到 ESP8266 的 RX。

ESP8266 傳送到 RFID RXD 的 3.3V 訊號，也需要確認讀寫器是否能正常辨識。

## 5. YPD-R200 UART 通訊設定

目前 RFID 使用的通訊速度是 115200 bps。

```cpp
rfid.begin(115200);
```

設定如下：

| 項目 | 設定 |
|---|---|
| Baud Rate | 115200 |
| Data Bits | 8 |
| Stop Bits | 1 |
| Parity | None |
| Flow Control | None |

目前使用 `SoftwareSerial` 與 RFID 通訊。

需要注意的是，ESP8266 使用 SoftwareSerial 跑 115200 bps 時，可能會受到程式執行時間或線路雜訊影響。

如果之後出現資料接收不完整、封包錯誤數量增加等情況，會優先從通訊速度、接線和供電部分檢查。

## 6. RFID 單次讀取指令

程式會每隔 300 ms 呼叫一次 `requestRFID()`，向 YPD-R200 發送單次盤點指令，詢問目前感應範圍內有沒有 RFID TAG。

```cpp
void requestRFID() {
    const uint8_t cmd[] = {
        0xAA, 0x00, 0x22,
        0x00, 0x00, 0x22, 0xDD
    };

    rfid.write(cmd, sizeof(cmd));
}
```

發送的十六進位資料：

```text
AA 00 22 00 00 22 DD
```

各個欄位的用途：

| 資料 | 說明 |
|---|---|
| AA | 封包開頭 |
| 00 | 指令封包 |
| 22 | 單次盤點指令 |
| 00 00 | 沒有額外參數 |
| 22 | Checksum |
| DD | 封包結尾 |

這組指令是參考 YPD-R200 的韌體指令手冊設定的。

## 7. RFID 資料接收方式

RFID 回傳的資料不是一次就能直接當成完整封包使用，所以程式會透過 `readRFID()`，把收到的資料一個 Byte、一個 Byte 存到緩衝區。

處理方式大致如下：

1. 先等待封包開頭 `0xAA`。
2. 收到前 5 個 Byte 後，取得資料長度。
3. 算出整個封包應該有多長。
4. 等待資料全部接收完成。
5. 將完整封包交給 `processFrame()` 檢查。

完整封包長度計算：

```cpp
total = payloadLength + 7;
```

另外，如果封包還沒收完，超過 100 ms 都沒有收到後續資料，就會清除目前的接收紀錄。

這樣可以避免某次資料接收失敗後，影響後續的 RFID 讀取。

## 8. RFID 封包檢查

這個部分主要由 `processFrame()` 負責。

因為 UART 收到的資料不一定每次都完整，所以不能直接把所有資料當成有效的 RFID TAG。

目前會檢查以下內容：

- 封包是不是以 `0xAA` 開始。
- 封包是不是以 `0xDD` 結束。
- Payload 長度是否正確。
- Checksum 有沒有符合。
- 是否為 EPC 通知封包。
- EPC 長度是否合理。

其中 EPC 通知封包使用：

```cpp
Type = 0x02
Command = 0x22
```

Checksum 的部分，是將 Type 到最後一個 Payload Byte 的資料相加，只保留最低 8 bit。

```cpp
uint8_t checksum = 0;

for (uint16_t i = 1; i < 5 + len; i++) {
    checksum += f[i];
}
```

只有通過這些檢查後，才會繼續解析 EPC。

## 9. EPC 長度判斷

YPD-R200 回傳的 EPC 通知封包中，Payload 主要包含：

```text
RSSI + PC + EPC + CRC
```

其中：

- RSSI：1 Byte
- PC：2 Bytes
- EPC：依標籤長度而定
- CRC：2 Bytes

所以程式可以先扣掉固定的 5 Bytes，得到 EPC 的實際長度。

```cpp
uint16_t epcBytes = len - 5;
```

接著再從 PC 資料中取得 EPC 應該有多少 Bytes。

```cpp
uint16_t expectedEpcBytes =
    ((pc >> 11) & 0x1F) * 2;
```

最後比較兩個長度是否一樣。

如果長度不符合，就不使用這筆資料。

## 10. RFID 三次確認機制

在設計計分功能時，有考慮到 RFID 可能因為讀取環境、訊號干擾等因素，偶爾讀到不穩定的資料。

因此沒有設計成讀到一次 TAG 就立刻加分，而是讓同一個 EPC 必須在短時間內確認三次。

目前設定：

```cpp
const uint8_t REQUIRED_READS = 3;

const unsigned long CONFIRM_MS = 2000;

const unsigned long MIN_CONFIRM_GAP_MS = 120;
```

判斷方式：

1. 第一次讀到 EPC，先將它記錄起來。
2. 下一次讀到相同 EPC，才繼續增加確認次數。
3. 每次有效確認至少間隔 120 ms。
4. 三次確認必須在 2 秒內完成。
5. 如果 EPC 不同，就重新記錄新的 EPC。
6. 確認三次成功後，分數加 1。

這樣可以減少因為單次讀取異常而造成誤判的情況。

這部分的判斷流程也會另外使用 draw.io 繪製。

## 11. 防止重複計分

一開始測試自動計分時，需要特別考慮一個問題：

如果無人機通過球門後，RFID TAG 還留在讀取範圍內，讀寫器可能會一直讀到同一個 EPC。

如果沒有額外處理，分數就可能一直增加。

因此程式加入 `scoringArmed` 變數，控制目前能不能加分。

```cpp
bool scoringArmed = true;
```

當三次確認成功後：

```cpp
scoringArmed = false;
addScore(epc);
```

分數加 1，同時將系統設為鎖定狀態。

之後就算繼續收到 RFID 資料：

```cpp
if (!scoringArmed) {
    return;
}
```

也不會再次加分。

## 12. RFID 自動解鎖

因為加分後系統會鎖定，所以還需要判斷什麼時候可以重新計分。

目前使用的方式是記錄最後一次讀到有效 EPC 的時間。

```cpp
lastValidSeen = millis();
```

當系統已經鎖定，而且連續 3 秒都沒有收到有效 EPC，就會自動解除鎖定。

```cpp
if (now - lastValidSeen >= AUTO_REARM_MS) {
    scoringArmed = true;
}
```

目前設定：

```cpp
const unsigned long AUTO_REARM_MS = 5000;
```

例如無人機進入 RFID 感應區並成功加分後，系統會先鎖定。

如果 TAG 還在感應範圍內，就不會繼續加分。

直到 TAG 離開，而且超過 3 秒沒有新的有效讀取，才會重新允許計分。

需要注意的是，這裡的 3 秒代表「沒有再讀到有效 EPC」，並不是直接測量無人機是否真的離開球門。

## 13. 七段顯示器的分數處理

目前使用兩位七段顯示器來顯示分數，最大可以顯示到 99。

因為 PCB 使用 74HC595 傳送資料，所以程式會先把分數拆成十位數和個位數。

例如現在分數是 27：

```cpp
score = 27;
```

先計算：

```cpp
byte tens = score / 10;
byte ones = score % 10;
```

結果：

```text
tens = 2
ones = 7
```

接著把兩個數字組成一個 Byte。

```cpp
byte data = (tens << 4) | ones;
```

得到：

```text
0010 0111
```

前面 4 bit 代表十位數，後面 4 bit 代表個位數。

最後使用：

```cpp
shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, data);
```

將資料傳送到 74HC595。

目前規劃的輸出分配：

| 74HC595 | 功能 |
|---|---|
| Q7～Q4 | 十位數 BCD |
| Q3～Q0 | 個位數 BCD |

接著再透過 PCB 上的 BCD 解碼與七段顯示驅動電路，顯示最後的分數。

這部分是沿用之前 PCB 七段顯示器的控制方式。

## 14. Serial Monitor 測試指令

為了方便測試，在程式中加入三個簡單的指令。

使用 Arduino IDE 或其他序列埠監控工具，將 Baud Rate 設為 115200，就可以傳送指令。

| 指令 | 功能 |
|---|---|
| R | 手動解除計分鎖定 |
| C | 分數清零 |
| S | 查看系統狀態 |

### R：手動解鎖

當系統處於鎖定狀態時，可以輸入 `R` 嘗試重新解鎖。

不過為了避免 TAG 還停留在感應範圍內，程式會檢查最近 3 秒是否讀到有效 EPC。

如果有，就不允許手動解鎖。

### C：分數清零

輸入 `C` 後，分數會回到 00，七段顯示器也會同步更新。

不過這個指令只會清除分數，不會改變 RFID 的鎖定狀態。

### S：查看系統狀態

輸入 `S` 可以查看：

```text
SCORE
ARMED
VALID FRAMES
INVALID FRAMES
LAST TAG SEEN
```

主要是測試 RFID 是否有正常讀取資料，以及目前系統是不是處於鎖定狀態。

## 15. 主程式執行流程

ESP8266 開機後，會先設定 GPIO、Serial Monitor 和 RFID 通訊。

完成初始化後，先將分數設定為 0，讓七段顯示器顯示 00。

接著進入 `loop()`，重複執行以下工作：

1. 接收並處理 RFID 資料。
2. 檢查是否符合自動解鎖條件。
3. 檢查 Serial Monitor 是否有輸入指令。
4. 檢查 EPC 確認時間是否超過 2 秒。
5. 每隔 300 ms 發送一次 RFID 盤點指令。
6. 重複執行以上工作。

整個程式主要利用 `millis()` 計算時間，不需要每隔一段時間就使用 `delay()` 暫停主程式。

## 16. 硬體接線整理

### ESP8266 與 PCB

```text
ESP8266 D5  → PCB DATA / DA
ESP8266 D6  → PCB CLOCK / CLK
ESP8266 D0  → PCB LATCH
ESP8266 GND → PCB GND
```

### ESP8266 與 YPD-R200

```text
ESP8266 D7 (RX) ← YPD-R200 TXD
ESP8266 D2 (TX) → YPD-R200 RXD

ESP8266 GND ──── YPD-R200 GND

5V 電源 ──────── YPD-R200 VCC
```

### 共地

ESP8266、PCB 和 RFID 讀寫器的 GND 必須連在一起。

如果沒有共地，可能會造成 UART 通訊不穩定，或七段顯示器無法正常更新。

另外，PCB 的七段顯示電路和 RFID 的供電會依照各自的需求處理，不能把高電壓電源直接接到 ESP8266 的 GPIO。

## 17. 測試方式

目前規劃依照以下順序進行整合測試。

**第一階段：PCB 顯示器測試**

先使用 ESP8266 控制 PCB，確認七段顯示器能正常顯示 00～99。

**第二階段：RFID 讀取測試**

先不接七段顯示器，只讓 ESP8266 與 YPD-R200 通訊。

使用 Serial Monitor 檢查是否能成功讀取 EPC。

**第三階段：自動計分測試**

將 RFID TAG 放到感應範圍內，觀察程式是否在確認三次後加 1 分。

**第四階段：防重複計分測試**

讓 TAG 持續停留在 RFID 感應範圍內，確認系統只會加 1 分，不會持續增加。

**第五階段：自動解鎖測試**

將 TAG 移開，等待超過 5 秒，檢查是否出現：

```text
AUTO REARMED
READY FOR NEXT TAG
```

再將 TAG 放回感應範圍，確認系統能重新計分。

**第六階段：整體測試**

最後再將 ESP8266、YPD-R200 和 PCB 七段顯示器全部接起來，測試 RFID 進球判斷與分數顯示能否同步運作。

## 18. 常見問題與檢查方式

### RFID 沒有讀到 EPC

如果 Serial Monitor 一直沒有顯示 EPC，可以先檢查：

- TX 和 RX 是否接反。
- ESP8266 和 RFID 是否共地。
- RFID 的 5V 電源是否穩定。
- UART 通訊速度是否設定正確。
- RFID 天線是否正常連接。

### INVALID FRAMES 數量增加

如果一直收到錯誤封包，可能是 UART 通訊出現問題。

目前使用 SoftwareSerial 搭配 115200 bps，所以會先檢查接線、供電和通訊速度。

另外也要確認 RFID 的 TXD 訊號電壓是否適合 ESP8266 使用。

### TAG 一直重複加分

正常情況下，成功加分後應該看到：

```text
TAG LOCKED
WAITING FOR TAG EXIT...
```

如果沒有進入鎖定狀態，就需要檢查 `scoringArmed` 是否有正常切換。

如果 TAG 一直被讀取，系統理論上會維持鎖定，不應該一直加分。

## 19. 程式主要參數

以下是目前使用的設定值，之後可以根據實際測試情況再進行調整。

| 參數 | 目前設定 | 用途 |
|---|---|---|
| RFID_POLL_MS | 300 ms | RFID 讀取間隔 |
| CONFIRM_MS | 2000 ms | EPC 確認時間 |
| MIN_CONFIRM_GAP_MS | 120 ms | 兩次有效確認的最小間隔 |
| REQUIRED_READS | 3 | EPC 確認次數 |
| AUTO_REARM_MS | 5000 ms | 自動解鎖等待時間 |
| MANUAL_REARM_MS | 3000 ms | 手動解鎖安全時間 |
| MAX_FRAME | 96 Bytes | RFID 封包緩衝區大小 |
| score | 00～99 | 分數顯示範圍 |

目前先使用這組參數進行測試，主要目的是讓 RFID 讀取與計分功能能夠正常運作。

之後如果發現 TAG 經過球門的速度太快，造成三次確認來不及完成，就需要再調整確認次數、讀取間隔或確認時間。

## 20. 自動計分演算法整理

這次自動計分程式主要是利用 RFID 讀取 EPC，來判斷是否符合計分條件。

ESP8266 會每隔 300 ms 向 YPD-R200 發送一次 RFID 盤點指令。收到回傳資料後，先檢查封包格式、長度和 Checksum，確認沒有問題後，再將 EPC 取出來進行判斷。

為了避免 RFID 偶爾讀到錯誤或不穩定的資料，目前設定同一個 EPC 必須在 2 秒內確認三次，才會讓分數加 1。

加分完成後，程式會先將計分功能鎖定。即使 TAG 還在感應範圍內，也不會一直重複加分。等到連續 5 秒都沒有再讀到有效 EPC，系統才會自動解除鎖定，準備下一次計分。

最後再將分數拆成十位數和個位數，轉換成 BCD 資料，透過 74HC595 傳送到 PCB 的七段顯示電路，讓顯示器可以更新目前的分數。

這個版本先以單一 RFID 讀寫器和兩位七段顯示器為主，後續再依照實際的無人機足球比賽需求，調整讀取範圍、判斷時間，以及其他計分功能。
