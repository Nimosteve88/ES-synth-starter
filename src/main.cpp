#include <Arduino.h>
#include <U8g2lib.h>
#include <bitset>

//Constants
  const uint32_t interval = 100; //Display update interval
  const uint32_t samplerate = 22050; //Audio sample rate

//Pin definitions
  //Row select and enable
  const int RA0_PIN = D3;
  const int RA1_PIN = D6;
  const int RA2_PIN = D12;
  const int REN_PIN = A5;

  //Matrix input and output
  const int C0_PIN = A2;
  const int C1_PIN = D9;
  const int C2_PIN = A6;
  const int C3_PIN = D1;
  const int OUT_PIN = D11;

  //Audio analogue out
  const int OUTL_PIN = A4;
  const int OUTR_PIN = A3;

  //Joystick analogue in
  const int JOYY_PIN = A0;
  const int JOYX_PIN = A1;

  //Output multiplexer bits
  const int DEN_BIT = 3;
  const int DRST_BIT = 4;
  const int HKOW_BIT = 5;
  const int HKOE_BIT = 6;

//Display driver object
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// Step sizes for 12-note equal temperament scale (A4 = 440Hz at index 9)
constexpr uint32_t calculateStepSize(float frequency) {
  return (uint32_t)((pow(2, 32) * frequency) / samplerate);
}

const uint32_t stepSizes[12] = {
  calculateStepSize(261.63),  // C4
  calculateStepSize(277.18),  // C#4
  calculateStepSize(293.66),  // D4
  calculateStepSize(311.13),  // D#4
  calculateStepSize(329.63),  // E4
  calculateStepSize(349.23),  // F4
  calculateStepSize(369.99),  // F#4
  calculateStepSize(392.00),  // G4
  calculateStepSize(415.30),  // G#4
  calculateStepSize(440.00),  // A4
  calculateStepSize(466.16),  // A#4
  calculateStepSize(493.88)   // B4
};

// Current step size (updated when key is pressed)
volatile uint32_t currentStepSize = 0;
const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };


//Function to set outputs using key matrix
void setOutMuxBit(const uint8_t bitIdx, const bool value) {
      digitalWrite(REN_PIN,LOW);
      digitalWrite(RA0_PIN, bitIdx & 0x01);
      digitalWrite(RA1_PIN, bitIdx & 0x02);
      digitalWrite(RA2_PIN, bitIdx & 0x04);
      digitalWrite(OUT_PIN,value);
      digitalWrite(REN_PIN,HIGH);
      delayMicroseconds(2);
      digitalWrite(REN_PIN,LOW);
}

void setup() {
  // put your setup code here, to run once:

  //Set pin directions
  pinMode(RA0_PIN, OUTPUT);
  pinMode(RA1_PIN, OUTPUT);
  pinMode(RA2_PIN, OUTPUT);
  pinMode(REN_PIN, OUTPUT);
  pinMode(OUT_PIN, OUTPUT);
  pinMode(OUTL_PIN, OUTPUT);
  pinMode(OUTR_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(C0_PIN, INPUT);
  pinMode(C1_PIN, INPUT);
  pinMode(C2_PIN, INPUT);
  pinMode(C3_PIN, INPUT);
  pinMode(JOYX_PIN, INPUT);
  pinMode(JOYY_PIN, INPUT);

  //Initialise display
  setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply

  //Initialise UART
  Serial.begin(9600);
  Serial.println("Hello World");
}

std::bitset<4> readCols() {
  // digitalWrite(RA0_PIN,LOW);
  // digitalWrite(RA1_PIN,LOW);
  // digitalWrite(RA2_PIN,LOW);
  // digitalWrite(REN_PIN,HIGH);

  std::bitset<4> cols;
  cols[0] = digitalRead(C0_PIN);
  cols[1] = digitalRead(C1_PIN);
  cols[2] = digitalRead(C2_PIN);
  cols[3] = digitalRead(C3_PIN);
  return cols;
}

void setRow(const uint8_t row) {
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA0_PIN, row & 0x01);
  digitalWrite(RA1_PIN, (row >> 1) & 0x01);
  digitalWrite(RA2_PIN, (row >> 2) & 0x01);
  delayMicroseconds(2);
  digitalWrite(REN_PIN, HIGH);
}


// Function to update step size based on key matrix
void updateStepSize(std::bitset<16> inputs) {
  currentStepSize = 0; // Default to 0 if no keys are pressed
  int lastKeyIndex = -1;

  for (int i = 0; i < 12; i++) {
      if (inputs[i]) {
          currentStepSize = stepSizes[i];
          lastKeyIndex = i;
      }
  }

  // Update OLED Display
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  
  if (lastKeyIndex != -1) {
      u8g2.setCursor(2, 10);
      u8g2.print("Note: ");
      u8g2.print(noteNames[lastKeyIndex]);
  } else {
      u8g2.setCursor(2, 10);
      u8g2.print("No Key Pressed");
  }

  u8g2.sendBuffer();
}

void loop() {
  static uint32_t next = millis();
  static uint32_t count = 0;
  std::bitset<16> inputs;  // Store 16-bit inputs (4 rows x 4 columns)

  while (millis() < next); // Wait for next interval
  next += interval;

  // Read matrix input
  for (uint8_t row = 0; row < 4; row++) {
      setRow(row);
      delayMicroseconds(2);
      std::bitset<4> rowInputs = readCols();
      
      // Store results in a 16-bit bitset
      for (uint8_t col = 0; col < 4; col++) {
          inputs[row * 4 + col] = rowInputs[col];
      }
  }
  updateStepSize(inputs);

  // Update display
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(2, 20);
  u8g2.print(inputs.to_ulong(), HEX);
  u8g2.sendBuffer();

  // Toggle LED
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}