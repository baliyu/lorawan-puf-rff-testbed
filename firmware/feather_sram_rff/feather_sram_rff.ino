#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

/* === HARDWARE PINS === */
const int PIN_MOSFET_GATE = 5;   // Controls power to SRAM
const int PIN_SRAM_CS_1   = 6;   // SRAM A
const int PIN_SRAM_CS_2   = 9;   // SRAM B

// LoRa Config (Feather M0 RFM95)
const int PIN_LORA_CS     = 8;
const int PIN_LORA_RST    = 4;
const int PIN_LORA_IRQ    = 3;
const float FREQUENCY     = 868.1E6; 

/* === SRAM SETTINGS === */
#define CMD_READ  0x03
static SPISettings SRAM_SPI(8000000, MSBFIRST, SPI_MODE0);
unsigned long OFF_TIME_MS = 2000; 
unsigned long ON_TIME_MS  = 50;   

volatile uint8_t selected_chip = 1; 
int capture_counter = 0;
bool lora_active = false;

/* === HELPER FUNCTIONS === */
void setBusHighZ() {
  pinMode(PIN_SRAM_CS_1, INPUT);
  pinMode(PIN_SRAM_CS_2, INPUT);
  pinMode(SCK, INPUT);
  pinMode(MOSI, INPUT);
  pinMode(MISO, INPUT);
}

void restoreBus() {
  pinMode(PIN_SRAM_CS_1, OUTPUT);
  pinMode(PIN_SRAM_CS_2, OUTPUT);
  digitalWrite(PIN_SRAM_CS_1, HIGH);
  digitalWrite(PIN_SRAM_CS_2, HIGH);
}

void powerCycleSRAM() {
  SPI.end();
  setBusHighZ();
  digitalWrite(PIN_MOSFET_GATE, LOW);
  delay(OFF_TIME_MS); 
  digitalWrite(PIN_MOSFET_GATE, HIGH);
  delay(5); 
  restoreBus();
  SPI.begin();
  delay(ON_TIME_MS);
}

uint8_t readSramByte(uint8_t chip, uint16_t address) {
  int cs_pin = (chip == 1) ? PIN_SRAM_CS_1 : PIN_SRAM_CS_2;
  uint8_t b;
  SPI.beginTransaction(SRAM_SPI);
  digitalWrite(cs_pin, LOW);
  SPI.transfer(CMD_READ);
  SPI.transfer((uint8_t)(address >> 8));
  SPI.transfer((uint8_t)address);
  b = SPI.transfer(0x00);
  digitalWrite(cs_pin, HIGH);
  SPI.endTransaction();
  return b;
}

/* === MAIN LOGIC === */
void performCapture() {
  capture_counter++;
  
  // 1. CAPTURE PUF (Prints to Python)
  Serial.println("=== SRAM PUF CAPTURE ===");
  Serial.print("Capture Number: "); Serial.println(capture_counter);
  Serial.print("Chip: SRAM"); Serial.println(selected_chip);
  Serial.println("Device: FeatherM0_LoRa");
  Serial.print("Timestamp(ms): "); Serial.println(millis());
  Serial.println();

  powerCycleSRAM(); 

  Serial.println("=== PUF DATA START ===");
  int ones = 0;
  int total = 0;
  for (uint32_t i = 0; i < 8192; i++) {
    if ((i % 256) == 0) Serial.println();
    uint8_t v = readSramByte(selected_chip, (uint16_t)i);
    for (int bit = 7; bit >= 0; --bit) {
      if (v & (1 << bit)) { Serial.print('1'); ones++; }
      else                { Serial.print('0'); }
      total++;
    }
    if (((i + 1) % 32) != 0) Serial.print(',');
    else Serial.println();
  }
  Serial.println();
  Serial.println("=== PUF DATA END ===");
  
  float bias = (float)ones / (float)total * 100.0;
  Serial.print("Bias: "); Serial.print(bias); Serial.println("%");
  Serial.println("=== PUF CAPTURE COMPLETE ===");

  // 2. TRANSMIT LORA (Signals the SDR)
  delay(50); 
  if (lora_active) {
      LoRa.beginPacket();
      LoRa.print("PUF_SYNC:");
      LoRa.print(capture_counter);
      LoRa.endPacket();
      Serial.println("--> LoRa Packet Sent.");
  } else {
      Serial.println("--> LoRa Skipped (Radio Failed Init)");
  }
  Serial.println();
}

void printStatus() {
  Serial.println("=== SYSTEM STATUS ===");
  Serial.print("LoRa Radio: "); Serial.println(lora_active ? "ACTIVE" : "FAILED");
  Serial.print("Selected Chip: "); Serial.println(selected_chip);
  Serial.println("Ready.");
}

void setup() {
  Serial.begin(115200);
  delay(3000); // Wait for you to open monitor
  Serial.println("--- FEATHER M0 HARDWARE INIT ---");

  // 1. SECURE THE SPI BUS (Crucial Step)
  // We must ensure the SRAM Chip Select pins are HIGH (Inactive) immediately.
  // If they float LOW, the SRAM will try to talk over the Radio.
  pinMode(PIN_SRAM_CS_1, OUTPUT); digitalWrite(PIN_SRAM_CS_1, HIGH);
  pinMode(PIN_SRAM_CS_2, OUTPUT); digitalWrite(PIN_SRAM_CS_2, HIGH);
  pinMode(PIN_LORA_CS, OUTPUT);   digitalWrite(PIN_LORA_CS, HIGH);

  // 2. POWER UP THE SRAM (The Fix)
  // We turn the SRAM ON so it stops dragging the SPI lines to ground.
  pinMode(PIN_MOSFET_GATE, OUTPUT);
  digitalWrite(PIN_MOSFET_GATE, HIGH); // <--- MUST BE HIGH!
  Serial.println("1. SRAM Powered ON & Deselected.");
  delay(50); // Give voltage time to stabilize

  // 3. MANUAL RADIO RESET (Safety Step)
  // Sometimes the radio needs a kick to wake up.
  Serial.println("2. Resetting LoRa Radio...");
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_RST, HIGH); delay(10);
  digitalWrite(PIN_LORA_RST, LOW);  delay(10);
  digitalWrite(PIN_LORA_RST, HIGH); delay(10);

  // 4. INITIALIZE RADIO
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_IRQ);
  if (!LoRa.begin(FREQUENCY)) {
    Serial.println("❌ LoRa FAILED. Bus Conflict or Antenna Missing.");
    lora_active = false;
    
    // ERROR DIAGNOSIS:
    // If you see this, unplug the SRAM MISO wire (Pin 11 or 12). 
    // If LoRa starts working, the SRAM is physically shorting the bus.
  } else {
    Serial.println("✅ LoRa SUCCEEDED.");
    lora_active = true;
    LoRa.setSyncWord(0x12);
    LoRa.setTxPower(23); // Maximum Power for Sniffer
  }

  SPI.begin(); // Start the bus for the main loop
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();

    // HANDLER: Capture Command
    if (cmd == 'C' || cmd == 'c') {
      // SMALL DELAY: Wait 10ms to see if Python sent a chip number (e.g., "C1")
      delay(10); 
      if (Serial.available()) {
        char next = Serial.peek(); // Look at next char without removing it yet
        if (next == '1' || next == '2') {
          char chipNum = Serial.read(); // Consume the number
          if (chipNum == '1') selected_chip = 1;
          if (chipNum == '2') selected_chip = 2;
          // Serial.print("Switched to Chip "); Serial.println(selected_chip); // Optional debug
        }
      }
      // NOW perform capture with the correct chip selected
      performCapture();
    }
    // HANDLER: Manual Chip Select (if typing '1' or '2' manually)
    else if (cmd == '1') { selected_chip = 1; Serial.println("Chip 1 Selected"); }
    else if (cmd == '2') { selected_chip = 2; Serial.println("Chip 2 Selected"); }
    // HANDLER: Status
    else if (cmd == 'S' || cmd == 's') printStatus(); 
  }
}