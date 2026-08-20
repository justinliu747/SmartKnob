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

// Standard 7-bit address for Arduino Wire library (No need to bitshift!)
#define MT6701_ADDR 0x06

#define SK_SERVICE_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a001"
#define SK_STATUS_UUID   "cba1d411-0e8f-4e5c-8a21-6f3c9b01a002"
#define SK_VOLUME_UUID   "cba1d411-0e8f-4e5c-8a21-6f3c9b01a003"
#define SK_TRIGGER_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a004"

// Motor and Driver
BLDCMotor motor = BLDCMotor(7);
BLDCDriver3PWM driver = BLDCDriver3PWM(IN1, IN2, IN3);
TFT_eSPI tft = TFT_eSPI();

// Re-link your custom sensor
GenericSensor sensor = GenericSensor(encoderGetAngle, encoderI2CInit);

// --- Profile Variables ---
float startAngle = 0;
float currentAngle = 0;

int pidLimit = 5;
int numDetents = 100;
float detentSize = 2.0f * PI / (float)numDetents;
float closestDetent = 0.0f;

int profile = 1;
int prevProfile = 1;

float userTorque = 0.0f;
float maxTorque = pidLimit;

int lastDetent = 0;
bool detentInitialized = false;
bool bleConnected = false;
bool bleWasConnected = false;
int lastNotifiedMode = -1;
int lastNotifiedDetent = -1;

volatile bool remapPending = false;
volatile uint8_t remapPercent = 0;
volatile bool triggerPending = false;
volatile uint8_t triggerValue = 1;

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

void setup() {
  Serial.begin(115200);
  delay(200); // USB serial after a reboot, so the next lines actually show up
  Serial.println();
  Serial.print("Reset reason: ");
  Serial.println(resetReasonText(esp_reset_reason()));
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  tft.init();
  tft.setRotation(0);
  drawScreen_1();
  Serial.println("TFT: GC9A01 init ok");

  SimpleFOCDebug::enable(&Serial);
  motor.useMonitoring(Serial);

  // Init MT6701 Encoder using your custom functions
  sensor.init();
  motor.linkSensor(&sensor);

  // Init driver
  driver.voltage_power_supply = 9;
  driver.init();
  motor.linkDriver(&driver);

  // Motor config
  motor.controller = MotionControlType::torque;
  motor.voltage_limit = 9;

  // Init PID settings (P and D set within modes)
  motor.PID_velocity.I = 0.00;
  motor.PID_velocity.limit = pidLimit;
  
  // Init motor & FOC (calibrates the motor automatically)
  motor.init();
  motor.initFOC();

  // Get init angle
  sensor.update();
  startAngle = sensor.getAngle();

  Serial.println("Motor ready!");
  Serial.println("Send '1', '2', or '3' to switch profiles. Send 's' for focus trigger, 'f' for focus off.");

  startBle();
  Serial.println("BLE: advertising as SmartKnob");
  Serial.print("Free heap after BLE begin: ");
  Serial.println(ESP.getFreeHeap());
}

void loop() {
  // Main FOC algorithm and sensor read
  motor.loopFOC();
  sensor.update();

  if (remapPending) {
    remapPending = false;
    applyVolumeRemap(remapPercent);
  }

  if (triggerPending) {
    triggerPending = false;
    notifyFocusTrigger(triggerValue);
  }

  if (bleConnected != bleWasConnected) {
    bleWasConnected = bleConnected;
    if (bleConnected) {
      notifyStatus();
    }
  }
  
  // Replace physical button with simple Serial input for testing
  if (Serial.available() > 0) {
    char inChar = Serial.read();
    if (inChar == '1') profile = 1;
    if (inChar == '2') profile = 2;
    if (inChar == '3') profile = 3;
    if (inChar == 's' || inChar == 'S') {
      triggerValue = 1;
      triggerPending = true;
    }
    if (inChar == 'f' || inChar == 'F') {
      triggerValue = 2;
      triggerPending = true;
    }
  }

  // Reset start angle after switching profile
  if (prevProfile != profile) {
    startAngle = sensor.getAngle();
    detentInitialized = false;
  }

  // CW from encoder perspective (EP) is +tive angle
  // CCW from user perspective (UP) is +tive angle
  currentAngle = -(sensor.getAngle() - startAngle);

  if (profile != lastNotifiedMode && bleConnected) {
    notifyStatus();
  }

  // ------------------------------------
  // PROFILE 1: SPRING
  // ------------------------------------
  if (profile == 1) {
    prevProfile = 1;
    motor.PID_velocity.P = 3;
    motor.PID_velocity.D = 0.00;
    
    float angleToCenter = 0 - currentAngle; // Center is 0 deg
    userTorque = motor.PID_velocity(angleToCenter);
  }

  // ------------------------------------
  // PROFILE 2: DETENT
  // ------------------------------------
  else if (profile == 2) {
    prevProfile = 2;
    motor.PID_velocity.P = 10;
    motor.PID_velocity.D = 0.05;
    
    closestDetent = round(currentAngle / detentSize) * detentSize;
    float angleToDetent = closestDetent - currentAngle;
    
    // Virtual bounds
    if (currentAngle < 0) {
      angleToDetent = 3 * (0 - currentAngle); 
    }
    if (currentAngle > 2.0f * PI) {
      angleToDetent = 3 * (2.0f * PI - currentAngle); 
    }
    
    userTorque = motor.PID_velocity(angleToDetent);
    maxTorque = detentSize / 2.0f * 10.0f;

    int detent = (int)round(currentAngle / detentSize);
    bool inRange = (currentAngle >= 0.0f && currentAngle <= 2.0f * PI);

    if (!detentInitialized) {
      lastDetent = detent;
      detentInitialized = true;
    } else if (detent != lastDetent) {
      if (inRange && bleConnected) {
        notifyStatus();
      }
      if (inRange) {
        lastDetent = detent;
      }
    }
  }

  // ------------------------------------
  // PROFILE 3: SWITCH
  // ------------------------------------
  else if (profile == 3) {
    prevProfile = 3;
    motor.PID_velocity.P = 7;
    motor.PID_velocity.D = 0.03;
    
    float angleToDetent = 0.0f - currentAngle;
    
    if (currentAngle >= PI / 4.0f) {
      angleToDetent = PI / 2.0f - currentAngle;
    }
    
    userTorque = motor.PID_velocity(angleToDetent);
  }

  // Flip the torque sent to the motor because it was in UP
  motor.move(-userTorque);
}
