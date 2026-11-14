
const int dataPin = D5;     // GPIO14 → PCB DA
const int latchPin = D0;    // GPIO16 → PCB LATCH
const int clockPin = D6;    // GPIO12 → PCB CLK

void showNumber(int num) {
  // 十位數與個位數
  byte tens = num / 10;
  byte ones = num % 10;

  // Q7 Q6 Q5 Q4 | Q3 Q2 Q1 Q0
  // D  C  B  A  | D  C  B  A
  // 十位數       | 個位數
  byte data = (tens << 4) | ones;

  digitalWrite(latchPin, LOW);

  // 使用原本成功的 MSBFIRST
  shiftOut(dataPin, clockPin, MSBFIRST, data);

  digitalWrite(latchPin, HIGH);
}

void setup() {
  pinMode(dataPin, OUTPUT);
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);

  digitalWrite(latchPin, HIGH);

  showNumber(0);  // 開機顯示 00
}

void loop() {
  for (int num = 0; num <= 99; num++) {

    showNumber(num);

    delay(1000);  // 每秒加 1
  }
}
