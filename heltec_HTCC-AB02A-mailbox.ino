#include "LoRaWan_APP.h"  // Heltec CubeCell library
#include "Arduino.h"

// CubeCell GPIO pins (check your specific board)
#define REED_HATCH_PIN  GPIO1
#define REED_DOOR_PIN   GPIO2

// LoRa configuration (Italy 868 MHz)
#define LORA_FREQUENCY  868000000UL  // Hz
#define LORA_SF         7           // Spreading Factor 7
#define LORA_BW         125E3       // Bandwidth 125 kHz
#define LORA_CR         5           // Coding Rate 4/5
#define LORA_POWER      22          // dBm (EU868 legal max: 25, device max: 22)

// Mailbox states
volatile bool hatch_changed = false;
volatile bool door_changed = false;
bool last_hatch_state = HIGH;
bool last_door_state = HIGH;
uint32_t last_event_time = 0;
const uint32_t DEBOUNCE_MS = 300;  // 300ms debounce (allows 0.3s events)

// Comment out for production to save power
#define ENABLE_SERIAL_DEBUG

// USER button (CubeCell has USER_KEY defined; fallback to GPIO0)
#ifdef USER_KEY
#define USER_BUTTON_PIN USER_KEY
#else
#define USER_BUTTON_PIN GPIO0
#endif

volatile bool user_button_pressed = false;

// Interrupt Service Routines (keep them fast!)
void hatchISR() {
  hatch_changed = true;
}

void doorISR() {
  door_changed = true;
}

void userButtonISR() {
  user_button_pressed = true;
}

void setup() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Mailbox Sensor Starting ===");
  Serial.flush();
  delay(10);  // Allow UART hardware to finish
#endif
  
  // Initialize GPIO pins with pull-ups
  pinMode(REED_HATCH_PIN, INPUT_PULLUP);
  pinMode(REED_DOOR_PIN, INPUT_PULLUP);
  pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
  
  // Read initial states
  last_hatch_state = digitalRead(REED_HATCH_PIN);
  last_door_state = digitalRead(REED_DOOR_PIN);
  
  // Attach interrupts on CHANGE (triggers on both rising and falling edges)
  attachInterrupt(REED_HATCH_PIN, hatchISR, CHANGE);
  attachInterrupt(REED_DOOR_PIN, doorISR, CHANGE);
  attachInterrupt(USER_BUTTON_PIN, userButtonISR, FALLING);
  
  // Initialize LoRa radio
  Radio.Init(NULL);
  Radio.SetPublicNetwork(false);  // Private network sync word 0x1424
  Radio.SetChannel(LORA_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, LORA_POWER, 0, 0,
                    LORA_SF, LORA_BW, LORA_CR,
                    8, 0, true, 0, 0, false);  // preamble=8, CRC=true, iqInverted=false
  Radio.Sleep();  // Put radio to sleep initially
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("LoRa initialized successfully!");
  Serial.println("Waiting for mailbox events...");
  Serial.printf("Hatch: %s, Door: %s\n",
                last_hatch_state ? "CLOSED" : "OPEN",
                last_door_state ? "CLOSED" : "OPEN");
  Serial.flush();
  delay(10);  // Allow UART hardware to finish
#endif
}

void loop() {
  // Check if an interrupt occurred
  if (hatch_changed || door_changed) {
    // Small delay to let the signal stabilize
    delay(50);
    
    // Read current states
    bool hatch_state = digitalRead(REED_HATCH_PIN);
    bool door_state = digitalRead(REED_DOOR_PIN);
    
    // Check if state actually changed (not just noise) and debounce
    bool state_changed = (hatch_state != last_hatch_state) || (door_state != last_door_state);
    bool debounce_ok = (millis() - last_event_time > DEBOUNCE_MS);
    
    if (state_changed && debounce_ok) {
      // Convert to open/closed (inverted due to pullup)
      bool hatch_open = !hatch_state;
      bool door_open = !door_state;
      
#ifdef ENABLE_SERIAL_DEBUG
      Serial.printf("Event! Hatch: %s, Door: %s\n",
                    hatch_open ? "OPEN" : "CLOSED",
                    door_open ? "OPEN" : "CLOSED");
      Serial.flush();
      delay(10);  // Allow UART hardware to finish
#endif
      
      // Send LoRa packet
      sendMailboxPacket(hatch_open, door_open);
      
      // Update states
      last_hatch_state = hatch_state;
      last_door_state = door_state;
      last_event_time = millis();
    }
    
    // Clear interrupt flags
    hatch_changed = false;
    door_changed = false;
  }
  
  // Check USER button press (polling, could be interrupt-based)
  if (user_button_pressed) {
    user_button_pressed = false;

#ifdef ENABLE_SERIAL_DEBUG
    Serial.println("USER button test: sending OPEN then CLOSED");
    Serial.flush();
    delay(10);
#endif

    sendMailboxPacket(true, true);   // OPEN
    delay(1000);
    sendMailboxPacket(false, false); // CLOSED
  }
  
  // Enter deep sleep - wakes on GPIO interrupt
  // This is the key to battery life: ~10µA vs ~3-5mA with delay()
  lowPowerHandler();
}

void sendMailboxPacket(bool hatch_open, bool door_open) {
  // Wake up radio for transmission
  Radio.Standby();
  
  // 5-byte packet: [NodeID][Type][Hatch][Door][Battery%]
  uint8_t packet[5];
  packet[0] = 0x01;                    // Node ID (mailbox)
  packet[1] = 0x01;                    // Message type: status
  packet[2] = hatch_open ? 1 : 0;      // Hatch state
  packet[3] = door_open ? 1 : 0;       // Door state
  packet[4] = readBatteryPercent();    // Battery %
  
  // Transmit (Heltec CubeCell SX1262)
  Radio.Send(packet, 5);
  
  // Wait for transmission to complete (typically <100ms at SF7)
  delay(100);
  
  // Put radio back to sleep
  Radio.Sleep();
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("LoRa packet transmitted!");
  Serial.flush();
  delay(10);  // Allow UART hardware to finish
#endif
}

uint8_t readBatteryPercent() {
  // CubeCell has built-in battery ADC
  // Read ADC (0-4095 for 0-3.6V)
  uint32_t adc = analogRead(ADC);
  
  // Convert to voltage (CubeCell specific)
  float voltage = (adc * 3.6) / 4095.0;
  
  // Li-SOCl2: 3.0V=0%, 3.6V=100%
  uint8_t percent = (voltage - 3.0) * 100.0 / 0.6;
  
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  
  return percent;
}
