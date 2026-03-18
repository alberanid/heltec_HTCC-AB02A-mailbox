#include "LoRaWan_APP.h"
#include "Arduino.h"

#define REED_HATCH_PIN  GPIO1
#define REED_DOOR_PIN   GPIO2

// LoRa configuration (Italy 868 MHz)
#define RF_FREQUENCY                                868000000 // Hz
#define TX_OUTPUT_POWER                             22        // dBm
#define LORA_BANDWIDTH                              0         // [0: 125 kHz,
                                                              //  1: 250 kHz,
                                                              //  2: 500 kHz,
                                                              //  3: Reserved]
#define LORA_SPREADING_FACTOR                       8         // [SF7..SF12]
#define LORA_CODINGRATE                             4         // [1: 4/5,
                                                              //  2: 4/6,
                                                              //  3: 4/7,
                                                              //  4: 4/8]
#define LORA_PREAMBLE_LENGTH                        8         // Same for Tx and Rx
#define LORA_SYMBOL_TIMEOUT                         0         // Symbols
#define LORA_FIX_LENGTH_PAYLOAD_ON                  false
#define LORA_IQ_INVERSION_ON                        false
#define TX_TIMEOUT_VALUE                            3000

// Mailbox states
volatile bool hatch_changed = false;
volatile bool door_changed = false;
bool last_hatch_state = HIGH;
bool last_door_state = HIGH;
uint32_t last_event_time = 0;
const uint32_t DEBOUNCE_MS = 300;  // 300ms debounce (allows 0.3s events)

static RadioEvents_t RadioEvents;
void OnTxDone( void );
void OnTxTimeout( void );

// Comment out for production to save power
#define ENABLE_SERIAL_DEBUG

// USER button (CubeCell has USER_KEY defined; fallback to GPIO0)
#ifdef USER_KEY
#define USER_BUTTON_PIN USER_KEY
#else
#define USER_BUTTON_PIN GPIO0
#endif

volatile bool user_button_pressed = false;

// Interrupt Service Routines
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
  delay(10);
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
#ifdef ENABLE_SERIAL_DEBUG
  RadioEvents.TxDone = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
#endif
  Radio.Init( &RadioEvents );
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig( MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                                   LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                                   LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                                   true, 0, 0, LORA_IQ_INVERSION_ON, TX_TIMEOUT_VALUE);
  Radio.Sleep();  // Put radio to sleep initially
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("LoRa initialized successfully!");
  Serial.println("Waiting for mailbox events...");
  Serial.printf("Hatch: %s, Door: %s\n",
                last_hatch_state ? "CLOSED" : "OPEN",
                last_door_state ? "CLOSED" : "OPEN");
  Serial.flush();
  delay(10);
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
      delay(10);
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

    // Read battery once and reuse for both test packets so values are consistent
    uint8_t bat = readBatteryPercent();
#ifdef ENABLE_SERIAL_DEBUG
    Serial.printf("USER test battery: %u%%\n", bat);
    Serial.flush();
    delay(10);
#endif
    sendMailboxPacket(true, true, bat);   // OPEN
    delay(1000);
    sendMailboxPacket(false, false, bat); // CLOSED
  }
  
  // Enter deep sleep - wakes on GPIO interrupt
  // This is the key to battery life: ~10µA vs ~3-5mA with delay()
  lowPowerHandler();
}

void sendMailboxPacket(bool hatch_open, bool door_open, uint8_t batteryPercent /*= 0xFF - read inside if not provided */) {
  // Wake up radio for transmission
  Radio.Standby();
  
  // 5-byte packet: [NodeID][Type][Hatch][Door][Battery%]
  uint8_t packet[5];
  packet[0] = 0x01;                    // Node ID (mailbox)
  packet[1] = 0x01;                    // Message type: mailbox status
  packet[2] = hatch_open ? 1 : 0;      // Hatch state
  packet[3] = door_open ? 1 : 0;       // Door state
  packet[4] = (batteryPercent == 0xFF) ? readBatteryPercent() : batteryPercent;
  
  // Transmit (Heltec CubeCell SX1262)
  Radio.Send(packet, 5);
  
  // Wait for transmission to complete (typically <100ms at SF7)
  delay(1000);
  
  // Put radio back to sleep
  Radio.Sleep();
  
#ifdef ENABLE_SERIAL_DEBUG
  Serial.println("LoRa packet transmitted!");
  Serial.flush();
  delay(10);
#endif
}

uint8_t readBatteryPercent() {
  // CubeCell has built-in battery ADC
  // Read ADC (0-4095 for 0-3.6V).
  // Strategy: discard the first sample (ADC mux / S/H settling), take two samples and average.
  uint32_t adc1 = analogRead(ADC);           // discard / warm-up
  delayMicroseconds(50);
  uint32_t adc2 = analogRead(ADC);
  delayMicroseconds(10);
  uint32_t adc3 = analogRead(ADC);
  uint32_t adc = (adc2 + adc3) / 2;

  // Convert to voltage (CubeCell specific)
  float voltage = (adc * 3.6f) / 4095.0f;

  // Li-SOCl2: 3.0V=0%, 3.6V=100%
  int percent = (int)roundf((voltage - 3.0f) * 100.0f / 0.6f);
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;

#ifdef ENABLE_SERIAL_DEBUG
  Serial.printf("ADC raw=%u (1=%u 2=%u 3=%u) -> %.3fV -> %d%%\n", (unsigned)adc, (unsigned)adc1, (unsigned)adc2, (unsigned)adc3, voltage, percent);
  Serial.flush();
#endif

  return (uint8_t)percent;
}


void OnTxDone( void ) {
  Serial.println("TX done!");
}

void OnTxTimeout( void ) {
    Radio.Sleep( );
    Serial.println("TX Timeout......");
}
