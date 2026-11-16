#include <bluefruit.h>

#define BUTTON_1_PIN D1
#define BUTTON_2_PIN D2
#define BUTTON_1_NAME "Button-D1"
#define BUTTON_2_NAME "Button-D2"
#define VBAT_ENABLE_PIN VBAT_ENABLE // (P0_14) Controls the onboard battery voltage divider
#define QSPI_CS_PIN PIN_QSPI_CS     // (P0_25) Onboard QSPI Flash Chip Select
#define QSPI_SCK_PIN PIN_QSPI_SCK
#define QSPI_IO0_PIN PIN_QSPI_IO0

uint8_t activeButtonPin = BUTTON_1_PIN;

const uint32_t CLICK_WINDOW_MS = 400;
const uint32_t BUTTON_DEBOUNCE_MS = 25;
const uint32_t LONG_PRESS_MS = 800;

// Forward declarations
bool sendBTHomePacket(uint8_t batteryPct, uint8_t buttonEvent, uint8_t buttonId);
bool setButtonBleIdentity(uint8_t buttonId);
uint8_t readBatteryPercentage();
uint8_t readButtonEvent();
bool waitForButtonRelease(uint32_t pressStartedAt);
void putFlashToSleep();
void goToDeepSleep();

void setup() {
  uint32_t resetReason = readResetReason();
  uint32_t buttonLatch = NRF_P0->LATCH;

  pinMode(BUTTON_1_PIN, INPUT_PULLUP);
  pinMode(BUTTON_2_PIN, INPUT_PULLUP);

  uint32_t button1Mask = 1UL << g_ADigitalPinMap[BUTTON_1_PIN];
  uint32_t button2Mask = 1UL << g_ADigitalPinMap[BUTTON_2_PIN];
  uint8_t buttonId = 0;
  bool wokeFromSystemOff = (resetReason & POWER_RESETREAS_OFF_Msk) != 0;

  if (wokeFromSystemOff && (buttonLatch & button1Mask)) {
    activeButtonPin = BUTTON_1_PIN;
    buttonId = 1;
  } else if (wokeFromSystemOff && (buttonLatch & button2Mask)) {
    activeButtonPin = BUTTON_2_PIN;
    buttonId = 2;
  } else if (digitalRead(BUTTON_1_PIN) == LOW) {
    activeButtonPin = BUTTON_1_PIN;
    buttonId = 1;
  } else if (digitalRead(BUTTON_2_PIN) == LOW) {
    activeButtonPin = BUTTON_2_PIN;
    buttonId = 2;
  }

  if (buttonId == 0) {
    goToDeepSleep();
    return;
  }

  uint8_t buttonEvent = readButtonEvent();
  uint8_t batteryPercent = readBatteryPercentage();

  if (!sendBTHomePacket(batteryPercent, buttonEvent, buttonId)) {
    goToDeepSleep();
    return;
  }
  goToDeepSleep();
}

void loop() {
  // Never reached because System OFF resets the chip upon wake-up
}

bool sendBTHomePacket(uint8_t batteryPct, uint8_t buttonEvent, uint8_t buttonId) {
  Bluefruit.begin();
  Bluefruit.autoConnLed(false);
  Bluefruit.setTxPower(4); // Max TX power (+4 dBm) for reliable range
  if (!setButtonBleIdentity(buttonId)) {
    return false;
  }
  Bluefruit.setName(buttonId == 1 ? BUTTON_1_NAME : BUTTON_2_NAME);

  // Build BTHome v2 Service Data Payload (UUID 0xFCD2)
  // Objects must be ordered from lowest Object ID to highest (0x01 then 0x3A)
  uint8_t serviceData[] = {
    0xD2, 0xFC,       // BTHome UUID (0xFCD2 in little-endian)
    0x44,             // Device Info: BTHome v2, unencrypted, trigger-based
    0x01, batteryPct, // Object ID 0x01: Battery percentage (uint8, 1% resolution)
    0x3A, buttonEvent  // Object ID 0x3A: Button event (0x01-0x04 = single/double/triple/long)
  };

  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, serviceData, sizeof(serviceData));
  
  // Keep the name in the primary advertisement so passive scanners can see it.
  Bluefruit.Advertising.addName();

  // Advertise rapidly (every 20 ms) for 1.5 seconds so Home Assistant catches it
  Bluefruit.Advertising.setInterval(BLE_ADV_INTERVAL_FAST_DFLT, BLE_ADV_INTERVAL_SLOW_DFLT); // 32 * 0.625 ms = 20 ms
  Bluefruit.Advertising.start(0);
  delay(1500);
  Bluefruit.Advertising.stop();
  return true;
}

bool setButtonBleIdentity(uint8_t buttonId) {
  ble_gap_addr_t address = Bluefruit.getAddr();
  address.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
  address.addr[5] = (address.addr[5] & 0x3F) | 0xC0;
  address.addr[0] = (address.addr[0] & 0xFC) | buttonId;
  return Bluefruit.setAddr(&address);
}

uint8_t readButtonEvent() {
  uint8_t clickCount = 1;
  uint32_t firstPressStartedAt = millis();
  if (waitForButtonRelease(firstPressStartedAt)) {
    return 0x04; // BTHome long press
  }

  while (clickCount < 3) {
    uint32_t waitStarted = millis();
    bool nextClickDetected = false;

    while (millis() - waitStarted < CLICK_WINDOW_MS) {
      if (digitalRead(activeButtonPin) == LOW) {
        uint32_t pressStartedAt = millis();
        delay(BUTTON_DEBOUNCE_MS);
        if (digitalRead(activeButtonPin) == LOW) {
          if (waitForButtonRelease(pressStartedAt)) {
            return 0x04; // BTHome long press
          }
          ++clickCount;
          nextClickDetected = true;
          break;
        }
      }
      delay(5);
    }

    if (!nextClickDetected) {
      break;
    }
  }

  // BTHome button event values: 0x01 single, 0x02 double, 0x03 triple.
  return clickCount;
}

bool waitForButtonRelease(uint32_t pressStartedAt) {
  while (true) {
    while (digitalRead(activeButtonPin) == LOW) {
      delay(5);
    }
    delay(BUTTON_DEBOUNCE_MS);
    if (digitalRead(activeButtonPin) == HIGH) {
      return millis() - pressStartedAt >= LONG_PRESS_MS;
    }
  }
}

uint8_t readBatteryPercentage() {
  // Enable the onboard voltage divider (active LOW)
  pinMode(VBAT_ENABLE_PIN, OUTPUT);
  digitalWrite(VBAT_ENABLE_PIN, LOW);
  delay(5); // Let voltage stabilize

  // Configure ADC to 12-bit resolution with 3.6V reference (0.6V internal * 6)
  analogReadResolution(12);
  analogReference(AR_INTERNAL_3_0); // Or default 3.6V depending on core version
  
  // Read VBAT pin (connected via 1M / 510k divider -> ratio ~2.961)
  int raw = analogRead(PIN_VBAT);
  
  // Disable voltage divider immediately to prevent battery drain during sleep
  pinMode(VBAT_ENABLE_PIN, INPUT);

  // Convert raw ADC reading to millivolts (3.0V ref * 2.961 divider / 4096)
  float voltage = (raw * 3.0f * 2.961f) / 4096.0f;

  // Map typical LiPo voltage (3.3V = 0%, 4.2V = 100%)
  if (voltage >= 4.20f) return 100;
  if (voltage <= 3.30f) return 0;
  return (uint8_t)(((voltage - 3.30f) / (4.20f - 3.30f)) * 100.0f);
}

void putFlashToSleep() {
  // Send the flash's SPI-compatible Deep Power-Down command over its QSPI pins.
  pinMode(QSPI_CS_PIN, OUTPUT);
  pinMode(QSPI_SCK_PIN, OUTPUT);
  pinMode(QSPI_IO0_PIN, OUTPUT);

  digitalWrite(QSPI_CS_PIN, HIGH);
  digitalWrite(QSPI_SCK_PIN, LOW);
  digitalWrite(QSPI_IO0_PIN, LOW);
  delayMicroseconds(1);

  digitalWrite(QSPI_CS_PIN, LOW);
  delayMicroseconds(1);
  for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
    digitalWrite(QSPI_IO0_PIN, (0xB9 & mask) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(QSPI_SCK_PIN, HIGH);
    delayMicroseconds(1);
    digitalWrite(QSPI_SCK_PIN, LOW);
  }
  digitalWrite(QSPI_CS_PIN, HIGH);
  digitalWrite(QSPI_IO0_PIN, LOW);
}

void goToDeepSleep() {
  putFlashToSleep();

  uint32_t button1Pin = g_ADigitalPinMap[BUTTON_1_PIN];
  uint32_t button2Pin = g_ADigitalPinMap[BUTTON_2_PIN];
  NRF_P0->LATCH = (1UL << button1Pin) | (1UL << button2Pin);
  nrf_gpio_cfg_sense_input(button1Pin, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
  nrf_gpio_cfg_sense_input(button2Pin, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

  delay(50);

  // Enter System OFF mode (~5 µA current draw)
  NRF_POWER->SYSTEMOFF = 1;
}
