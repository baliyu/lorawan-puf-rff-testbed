#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

/* === HARDWARE PINS === */
const int PIN_MOSFET_GATE = 5;   // D5 (PB7)
const int PIN_SRAM_CS_1   = 10;  // D10 (PB6)
const int PIN_SRAM_CS_2   = 9;   // D9 (PB12)

// Internal LoRa Radio Pins (B-L072Z-LRWAN1)
// We use the pin names directly.
#define RADIO_MOSI  PA7
#define RADIO_MISO  PA6
#define RADIO_SCLK  PB3
#define RADIO_CS    PA15
#define RADIO_RST   PC0
#define RADIO_DIO0  PB4
const float FREQUENCY = 868.1E6; 

// === Create a SECOND SPI Bus for SRAM (SPI2) ===
// We use the specific pins for the External Header
// MOSI=D11 (PB15), MISO=D12 (PB14), SCK=D3 (PB13)
SPIClass sramSPI(11, 12, 3); 

#define CMD_READ  0x03
static SPISettings SRAM_SETTINGS(8000000, MSBFIRST, SPI_MODE0);
unsigned long OFF_TIME_MS = 4000; // Increased to 4s to help drain power
unsigned long ON_TIME_MS  = 50;   

volatile uint8_t selected_chip = 1; 
int capture_counter = 0;
bool lora_active = false;

/* === HELPER FUNCTIONS === */
void setBusHighZ() {
  // Just float the pins. Safe mode.
  pinMode(3, INPUT);  
  pinMode(11, INPUT); 
  pinMode(12, INPUT); 
  pinMode(PIN_SRAM_CS_1, INPUT);
  pinMode(PIN_SRAM_CS_2, INPUT);
}

void restoreBus() {
  pinMode(PIN_SRAM_CS_1, OUTPUT);
  pinMode(PIN_SRAM_CS_2, OUTPUT);
  digitalWrite(PIN_SRAM_CS_1, HIGH);
  digitalWrite(PIN_SRAM_CS_2, HIGH);
}

void powerCycleSRAM() {
  sramSPI.end(); // Stop SRAM bus
  setBusHighZ();
  
  digitalWrite(PIN_MOSFET_GATE, LOW);
  delay(OFF_TIME_MS); 
  digitalWrite(PIN_MOSFET_GATE, HIGH);
  delay(10); 
  
  restoreBus();
  sramSPI.begin(); // Restart SRAM bus
  delay(ON_TIME_MS);
}

uint8_t readSramByte(uint8_t chip, uint16_t address) {
  int cs_pin = (chip == 1) ? PIN_SRAM_CS_1 : PIN_SRAM_CS_2;
  uint8_t b;
  
  sramSPI.beginTransaction(SRAM_SETTINGS);
  digitalWrite(cs_pin, LOW);
  sramSPI.transfer(CMD_READ);
  sramSPI.transfer((uint8_t)(address >> 8));
  sramSPI.transfer((uint8_t)address);
  b = sramSPI.transfer(0x00);
  digitalWrite(cs_pin, HIGH);
  sramSPI.endTransaction();
  return b;
}

void performCapture() {
  capture_counter++;
  
  Serial.println("=== SRAM PUF CAPTURE ===");
  Serial.print("Capture Number: "); Serial.println(capture_counter);
  Serial.print("Chip: SRAM"); Serial.println(selected_chip);
  Serial.println("Device: STM32_LoRa");
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

  delay(50); 
  if (lora_active) {
      Serial.println("Transmitting LoRa Sync Packet...");
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
  pinMode(PIN_MOSFET_GATE, OUTPUT);
  digitalWrite(PIN_MOSFET_GATE, HIGH); 
  delay(100); 

  restoreBus(); 

  Serial.begin(115200);
  
  // 1. Initialize SRAM Bus (External Pins)
  sramSPI.begin(); 

  // 2. Initialize LoRa Bus (Internal Pins)
  Serial.println("Initializing LoRa...");
  
  // FORCE the global SPI to use the Internal Radio pins
  SPI.setMOSI(RADIO_MOSI);
  SPI.setMISO(RADIO_MISO);
  SPI.setSCLK(RADIO_SCLK);
  SPI.begin(); 

  LoRa.setPins(RADIO_CS, RADIO_RST, RADIO_DIO0);
  
  if (!LoRa.begin(FREQUENCY)) {
    Serial.println("LoRa Init Failed!");
    lora_active = false;
  } else {
    Serial.println("LoRa Initialized OK!");
    lora_active = true;
    LoRa.setSyncWord(0x12); 
  }
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'C' || cmd == 'c') performCapture();
    else if (cmd == '1') { selected_chip = 1; Serial.println("Selected Chip 1"); }
    else if (cmd == '2') { selected_chip = 2; Serial.println("Selected Chip 2"); }
    else if (cmd == 'S' || cmd == 's') printStatus();
  }
}