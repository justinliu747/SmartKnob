#include <Wire.h> // Include Wire first
#include <SimpleFOC.h>
#include <TFT_eSPI.h>
#include <NimBLEDevice.h>
#include "esp_system.h"

// --- ESP32 Pin Definitions ---
#define IN1 2
#define IN2 3
#define IN3 4
#define I2C_SDA 13
#define I2C_SCL 12
#define BTN_PIN 10

// Standard 7-bit address for Arduino Wire library (No need to bitshift!)
#define MT6701_ADDR 0x06

#define SK_SERVICE_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a001"
#define SK_STATUS_UUID   "cba1d411-0e8f-4e5c-8a21-6f3c9b01a002"
#define SK_VOLUME_UUID   "cba1d411-0e8f-4e5c-8a21-6f3c9b01a003"
#define SK_TRIGGER_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a004"

#define TRIGGER_FOCUS_ON  1
#define TRIGGER_FOCUS_OFF 2
#define TRIGGER_PLAY_PAUSE 3

#define SCREEN_MENU    0
#define SCREEN_VOLUME  1
#define SCREEN_FOCUS   2
#define SCREEN_DAVINCI 3

#define FOCUS_IDLE    0
#define FOCUS_RUNNING 1
#define FOCUS_DONE    2

#define MENU_COUNT 3
#define FOCUS_MAX_MINUTES 99
#define MENU_CLICK_SIZE (2.0f * PI / 12.0f)
#define DAVINCI_TRIM_SIZE (2.0f * PI / 180.0f)

#define HAPTIC_NONE          0
#define HAPTIC_MENU          1
#define HAPTIC_VOLUME        2
#define HAPTIC_FOCUS_CONFIG  3
#define HAPTIC_SPRING        4
#define HAPTIC_DAVINCI       5
#define HAPTIC_DAVINCI_TRIM  6

#define BTN_NONE   0
#define BTN_SHORT  1
#define BTN_LONG   2
#define BTN_DOUBLE 3
#define BTN_DEBOUNCE_MS 40
#define BTN_LONG_MS 700
#define BTN_DOUBLE_MS 400

// Other .ino files are concatenated after this one; globals here need prototypes.
float encoderGetAngle();
void encoderI2CInit();
void startBle();
void applyVolumeRemap(uint8_t percent);
void notifyFocusTrigger(uint8_t value);
void notifyStatus();
uint8_t percentFromLevel(int level);
int detentLevel();
void updateUi();

// Motor and Driver
BLDCMotor motor = BLDCMotor(7);
BLDCDriver3PWM driver = BLDCDriver3PWM(IN1, IN2, IN3);
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
bool spriteOk = false;

// Re-link your custom sensor
GenericSensor sensor = GenericSensor(encoderGetAngle, encoderI2CInit);

// --- Profile Variables ---
float startAngle = 0;
float currentAngle = 0;

int pidLimit = 5;
int numDetents = 100;
float detentSize = 2.0f * PI / (float)numDetents;
float closestDetent = 0.0f;

int profile = 2;
int prevProfile = 2;

float userTorque = 0.0f;
float maxTorque = pidLimit;

int lastDetent = 0;
bool detentInitialized = false;
bool wrapDetents = true;
bool bleConnected = false;
bool bleWasConnected = false;
int lastNotifiedMode = -1;
int lastNotifiedDetent = -1;

volatile bool remapPending = false;
volatile uint8_t remapPercent = 0;
volatile bool davinciTrim = false;
volatile bool triggerPending = false;
volatile uint8_t triggerValue = 1;
volatile int hapticPending = HAPTIC_NONE;
volatile bool statusNotifyPending = false;
volatile bool uiDirty = true;

int uiScreen = SCREEN_MENU;
int focusPhase = FOCUS_IDLE;
int menuIndex = 0;
int focusMinutes = 0;
uint8_t lastPcVolume = 0;

unsigned long focusStartMs = 0;
unsigned long focusTargetMs = 0;
int focusElapsedMinutes = 0;
int lastDrawnFocusSec = -1;

bool btnLastRaw = HIGH;
bool btnStable = HIGH;
unsigned long btnLastChangeMs = 0;
unsigned long btnPressStartMs = 0;
bool btnLongFired = false;
bool btnAwaitDouble = false;
unsigned long btnFirstClickMs = 0;

NimBLECharacteristic* statusChar = nullptr;
NimBLECharacteristic* triggerChar = nullptr;
NimBLEServer* knobServer = nullptr;

const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW (software reset)";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dip)";
    case ESP_RST_USB:      return "USB";
    default:               return "OTHER";
  }
}

void setDetentCount(int n) {
  if (n < 1) {
    n = 1;
  }
  numDetents = n;
  detentSize = 2.0f * PI / (float)numDetents;
}

int wrappedIndex(int raw) {
  int n = numDetents;
  if (n <= 0) {
    return 0;
  }
  int idx = raw % n;
  if (idx < 0) {
    idx += n;
  }
  return idx;
}

void remapToLevel(int level) {
  if (level < 0) {
    level = 0;
  }
  if (wrapDetents) {
    level = wrappedIndex(level);
  } else if (level > numDetents) {
    level = numDetents;
  }
  float targetAngle = (float)level * detentSize;
  startAngle = sensor.getAngle() + targetAngle;
  currentAngle = targetAngle;
  lastDetent = level;
  detentInitialized = true;
}

void applyHaptic(int kind) {
  if (kind == HAPTIC_MENU) {
    wrapDetents = true;
    numDetents = MENU_COUNT;
    detentSize = MENU_CLICK_SIZE;
    remapToLevel(menuIndex);
  } else if (kind == HAPTIC_VOLUME) {
    wrapDetents = false;
    setDetentCount(100);
    applyVolumeRemap(lastPcVolume);
  } else if (kind == HAPTIC_FOCUS_CONFIG) {
    wrapDetents = false;
    setDetentCount(FOCUS_MAX_MINUTES);
    remapToLevel(focusMinutes);
  } else if (kind == HAPTIC_SPRING) {
    wrapDetents = false;
    startAngle = sensor.getAngle();
    currentAngle = 0;
    detentInitialized = false;
  } else if (kind == HAPTIC_DAVINCI || kind == HAPTIC_DAVINCI_TRIM) {
    wrapDetents = true;
    detentSize = (kind == HAPTIC_DAVINCI_TRIM) ? DAVINCI_TRIM_SIZE : MENU_CLICK_SIZE;
    startAngle = sensor.getAngle();
    currentAngle = 0;
    lastDetent = 0;
    detentInitialized = true;
    statusNotifyPending = true;
  }
}

unsigned long focusRemainingMs() {
  if (focusPhase != FOCUS_RUNNING) {
    return 0;
  }
  unsigned long elapsed = millis() - focusStartMs;
  if (elapsed >= focusTargetMs) {
    return 0;
  }
  return focusTargetMs - elapsed;
}

void enterMenu() {
  uiScreen = SCREEN_MENU;
  profile = 2;
  davinciTrim = false;
  uiDirty = true;
  hapticPending = HAPTIC_MENU;
}

void enterVolume() {
  uiScreen = SCREEN_VOLUME;
  profile = 2;
  uiDirty = true;
  hapticPending = HAPTIC_VOLUME;
}

void enterDavinci() {
  uiScreen = SCREEN_DAVINCI;
  profile = 4;
  davinciTrim = false;
  uiDirty = true;
  hapticPending = HAPTIC_DAVINCI;
}

void enterFocus() {
  uiScreen = SCREEN_FOCUS;
  if (focusPhase == FOCUS_RUNNING) {
    profile = 1;
    lastDrawnFocusSec = -1;
    hapticPending = HAPTIC_SPRING;
  } else {
    profile = 2;
    hapticPending = HAPTIC_FOCUS_CONFIG;
  }
  uiDirty = true;
}

void startFocusSession() {
  if (focusMinutes <= 0) {
    return;
  }
  focusPhase = FOCUS_RUNNING;
  focusTargetMs = (unsigned long)focusMinutes * 60000UL;
  focusStartMs = millis();
  lastDrawnFocusSec = -1;
  notifyFocusTrigger(TRIGGER_FOCUS_ON);
  enterFocus();
}

void endFocusSession() {
  if (focusPhase != FOCUS_RUNNING) {
    return;
  }
  unsigned long elapsed = millis() - focusStartMs;
  focusElapsedMinutes = (int)(elapsed / 60000UL);
  focusPhase = FOCUS_DONE;
  notifyFocusTrigger(TRIGGER_FOCUS_OFF);
  if (uiScreen == SCREEN_FOCUS) {
    profile = 1;
    hapticPending = HAPTIC_SPRING;
  }
  uiDirty = true;
}

void dismissOverlay() {
  focusPhase = FOCUS_IDLE;
  focusMinutes = 0;
  lastDrawnFocusSec = -1;
  if (uiScreen == SCREEN_FOCUS) {
    enterFocus();
  } else {
    uiDirty = true;
  }
}

void tickFocusSession() {
  if (focusPhase != FOCUS_RUNNING) {
    return;
  }
  unsigned long elapsed = millis() - focusStartMs;
  if (elapsed >= focusTargetMs) {
    endFocusSession();
    return;
  }
  if (uiScreen == SCREEN_FOCUS) {
    int remainingSec = (int)((focusTargetMs - elapsed) / 1000UL);
    if (remainingSec != lastDrawnFocusSec) {
      lastDrawnFocusSec = remainingSec;
      uiDirty = true;
    }
  }
}

int pollButton() {
  unsigned long now = millis();
  bool raw = digitalRead(BTN_PIN);

  if (raw != btnLastRaw) {
    btnLastRaw = raw;
    btnLastChangeMs = now;
  }

  if ((now - btnLastChangeMs) >= BTN_DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    if (btnStable == LOW) {
      btnPressStartMs = now;
      btnLongFired = false;
    } else if (!btnLongFired) {
      if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_RUNNING) {
        if (btnAwaitDouble && (now - btnFirstClickMs) <= BTN_DOUBLE_MS) {
          btnAwaitDouble = false;
          return BTN_DOUBLE;
        }
        btnAwaitDouble = true;
        btnFirstClickMs = now;
        return BTN_NONE;
      }
      return BTN_SHORT;
    }
  }

  if (btnStable == LOW && !btnLongFired && (now - btnPressStartMs) >= BTN_LONG_MS) {
    btnLongFired = true;
    btnAwaitDouble = false;
    return BTN_LONG;
  }

  if (btnAwaitDouble && (now - btnFirstClickMs) > BTN_DOUBLE_MS) {
    btnAwaitDouble = false;
  }

  return BTN_NONE;
}

void handleButton(int ev) {
  if (ev == BTN_NONE) {
    return;
  }

  if (focusPhase == FOCUS_DONE) {
    if (ev == BTN_SHORT) {
      dismissOverlay();
    } else if (ev == BTN_LONG) {
      enterMenu();
    }
    return;
  }

  if (ev == BTN_LONG) {
    if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_IDLE) {
      focusMinutes = 0;
    }
    enterMenu();
    return;
  }

  if (ev == BTN_DOUBLE) {
    if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_RUNNING) {
      endFocusSession();
    }
    return;
  }

  if (ev == BTN_SHORT) {
    if (uiScreen == SCREEN_MENU) {
      if (menuIndex == 0) {
        enterVolume();
      } else if (menuIndex == 1) {
        enterFocus();
      } else {
        enterDavinci();
      }
    } else if (uiScreen == SCREEN_VOLUME) {
      notifyFocusTrigger(TRIGGER_PLAY_PAUSE);
    } else if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_IDLE) {
      startFocusSession();
    } else if (uiScreen == SCREEN_DAVINCI) {
      davinciTrim = !davinciTrim;
      uiDirty = true;
      hapticPending = davinciTrim ? HAPTIC_DAVINCI_TRIM : HAPTIC_DAVINCI;
    }
  }
}

void onDetentChanged() {
  if (uiScreen == SCREEN_MENU) {
    int idx = wrappedIndex(lastDetent);
    if (idx != menuIndex) {
      menuIndex = idx;
      uiDirty = true;
    }
    return;
  }
  if (uiScreen == SCREEN_DAVINCI) {
    statusNotifyPending = true;
    return;
  }
  if (uiScreen == SCREEN_VOLUME) {
    lastPcVolume = percentFromLevel(detentLevel());
    uiDirty = true;
    statusNotifyPending = true;
  } else if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_IDLE) {
    focusMinutes = detentLevel();
    if (focusMinutes > FOCUS_MAX_MINUTES) {
      focusMinutes = FOCUS_MAX_MINUTES;
    }
    uiDirty = true;
  }
}

void runSpring() {
  prevProfile = 1;
  motor.PID_velocity.P = 3;
  motor.PID_velocity.D = 0.00;
  float angleToCenter = 0 - currentAngle;
  userTorque = motor.PID_velocity(angleToCenter);
}

void runDetents(bool wrap) {
  prevProfile = 2;
  motor.PID_velocity.P = 10;
  motor.PID_velocity.D = 0.05;

  closestDetent = round(currentAngle / detentSize) * detentSize;
  float angleToDetent = closestDetent - currentAngle;

  if (!wrap) {
    if (currentAngle < 0) {
      angleToDetent = 3 * (0 - currentAngle);
    }
    if (currentAngle > 2.0f * PI) {
      angleToDetent = 3 * (2.0f * PI - currentAngle);
    }
  }

  userTorque = motor.PID_velocity(angleToDetent);
  maxTorque = detentSize / 2.0f * 10.0f;

  int detent = (int)round(currentAngle / detentSize);
  bool inRange = wrap || (currentAngle >= 0.0f && currentAngle <= 2.0f * PI);

  if (!detentInitialized) {
    lastDetent = detent;
    detentInitialized = true;
  } else if (detent != lastDetent && inRange) {
    lastDetent = detent;
    onDetentChanged();
  }
}

void runSwitch() {
  prevProfile = 3;
  motor.PID_velocity.P = 7;
  motor.PID_velocity.D = 0.03;
  float angleToDetent = 0.0f - currentAngle;
  if (currentAngle >= PI / 4.0f) {
    angleToDetent = PI / 2.0f - currentAngle;
  }
  userTorque = motor.PID_velocity(angleToDetent);
}

void uiTask(void* pv) {
  for (;;) {
    if (triggerPending) {
      triggerPending = false;
      notifyFocusTrigger(triggerValue);
    }

    if (bleConnected != bleWasConnected) {
      bleWasConnected = bleConnected;
      if (bleConnected && (uiScreen == SCREEN_VOLUME || uiScreen == SCREEN_DAVINCI)) {
        statusNotifyPending = true;
      }
    }

    if (statusNotifyPending) {
      statusNotifyPending = false;
      notifyStatus();
    }

    if (Serial.available() > 0) {
      char inChar = Serial.read();
      if (inChar == '1') profile = 1;
      if (inChar == '2') profile = 2;
      if (inChar == '3') profile = 3;
      if (inChar == 's' || inChar == 'S') {
        triggerValue = TRIGGER_FOCUS_ON;
        triggerPending = true;
      }
      if (inChar == 'f' || inChar == 'F') {
        triggerValue = TRIGGER_FOCUS_OFF;
        triggerPending = true;
      }
    }

    handleButton(pollButton());
    tickFocusSession();
    updateUi();
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("SmartKnob");
  Serial.print("reset=");
  Serial.print(resetReasonText(esp_reset_reason()));
  Serial.print("  heap=");
  Serial.println(ESP.getFreeHeap());

  pinMode(BTN_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  sensor.init();
  motor.linkSensor(&sensor);

  driver.voltage_power_supply = 9;
  driver.init();
  motor.linkDriver(&driver);

  motor.controller = MotionControlType::torque;
  motor.voltage_limit = 9;

  motor.PID_velocity.I = 0.00;
  motor.PID_velocity.limit = pidLimit;

  motor.init();
  motor.initFOC();

  sensor.update();
  startAngle = sensor.getAngle();

  startBle();

  if (spr.createSprite(240, 240) == nullptr) {
    Serial.println("TFT sprite alloc failed");
  } else {
    spriteOk = true;
  }

  Serial.print("TFT ok  motor ok  BLE advertising  ");
  Serial.println(spriteOk ? "sprite ok" : "sprite FAILED");

  enterMenu();
  applyHaptic(HAPTIC_MENU);
  hapticPending = HAPTIC_NONE;
  updateUi();

  xTaskCreatePinnedToCore(uiTask, "ui", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  motor.loopFOC();
  sensor.update();

  if (remapPending) {
    remapPending = false;
    if (uiScreen == SCREEN_VOLUME) {
      lastPcVolume = remapPercent;
      if (lastPcVolume > 100) {
        lastPcVolume = 100;
      }
      applyVolumeRemap(lastPcVolume);
      uiDirty = true;
    }
  }

  int haptic = hapticPending;
  if (haptic != HAPTIC_NONE) {
    hapticPending = HAPTIC_NONE;
    applyHaptic(haptic);
  }

  // CW from encoder perspective (EP) is +tive angle
  // CCW from user perspective (UP) is +tive angle
  currentAngle = -(sensor.getAngle() - startAngle);

  if (uiScreen == SCREEN_MENU || uiScreen == SCREEN_DAVINCI) {
    runDetents(true);
  } else if (uiScreen == SCREEN_VOLUME) {
    runDetents(false);
  } else if (uiScreen == SCREEN_FOCUS && focusPhase == FOCUS_IDLE) {
    runDetents(false);
  } else if (profile == 3) {
    runSwitch();
  } else {
    runSpring();
  }

  motor.move(-userTorque);
}
