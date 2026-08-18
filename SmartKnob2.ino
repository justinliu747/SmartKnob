#include <Wire.h> // Include Wire first
#include <SimpleFOC.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "esp_system.h"

// --- ESP32 Pin Definitions ---
#define IN1 2
#define IN2 3
#define IN3 4
#define I2C_SDA 13
#define I2C_SCL 12

// Standard 7-bit address for Arduino Wire library (No need to bitshift!)
#define MT6701_ADDR 0x06

#define SK_SERVICE_UUID "cba1d411-0e8f-4e5c-8a21-6f3c9b01a001"
#define SK_STATUS_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a002"
#define SK_VOLUME_UUID  "cba1d411-0e8f-4e5c-8a21-6f3c9b01a003"

// Motor and Driver
BLDCMotor motor = BLDCMotor(7);
BLDCDriver3PWM driver = BLDCDriver3PWM(IN1, IN2, IN3);

// --- Custom MT6701 I2C Implementation for ESP32 ---
void encoderI2CInit() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); // 400kHz fast mode
}

float encoderGetAngle() {
  // Tell encoder we want to read starting from register 0x03
  Wire.beginTransmission(MT6701_ADDR);
  Wire.write(0x03);
  Wire.endTransmission(false); // 'false' sends a restart message to keep the connection active

  // Request 2 bytes (0x03 and 0x04)
  Wire.requestFrom(MT6701_ADDR, 2);

  if (Wire.available() >= 2) {
    uint8_t upper = Wire.read(); // bits [13:6]
    uint8_t lower = Wire.read(); // bits [5:0] in top 6 bits
    
    // Combine into 14-bit value just like your STM32 code
    uint16_t rawData = ((uint16_t)upper << 6) | (lower >> 2);
    return ((float)rawData / 16384.0f) * 2.0f * PI; 
  }
  
  return 0.0f; // Fallback if I2C fails
}

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
uint16_t hidConnHandle = 0xFFFF;
int lastNotifiedMode = -1;
int lastNotifiedDetent = -1;

volatile bool remapPending = false;
volatile uint8_t remapPercent = 0;

NimBLECharacteristic* statusChar = nullptr;
NimBLEServer* knobServer = nullptr;
NimBLEHIDDevice* hidDevice = nullptr;
NimBLECharacteristic* hidInputChar = nullptr;

// Minimal boot-keyboard report map (report ID 1).
uint8_t hidReportMap[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08,
  0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
  0x29, 0x65, 0x81, 0x00, 0xC0
};

void startKnobAdvertising();

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

int detentLevel() {
  int level = (int)round(currentAngle / detentSize);
  if (level < 0) level = 0;
  if (level > numDetents) level = numDetents;
  return level;
}

uint8_t percentFromLevel(int level) {
  return (uint8_t)((level * 100) / numDetents);
}

void notifyStatus() {
  if (statusChar == nullptr || !bleConnected) {
    return;
  }
  int level = detentLevel();
  uint8_t packet[4];
  packet[0] = (uint8_t)profile;
  packet[1] = (uint8_t)level;
  packet[2] = percentFromLevel(level);
  packet[3] = (uint8_t)numDetents;
  statusChar->setValue(packet, 4);
  statusChar->notify();
  lastNotifiedMode = profile;
  lastNotifiedDetent = level;
}

void applyVolumeRemap(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  float targetAngle = (percent / 100.0f) * 2.0f * PI;
  // currentAngle = -(sensor.getAngle() - startAngle) = startAngle - sensor.getAngle()
  startAngle = sensor.getAngle() + targetAngle;
  currentAngle = targetAngle;
  lastDetent = detentLevel();
  detentInitialized = true;
  Serial.print("BLE: remapped walls so this pose is ");
  Serial.print(lastDetent);
  Serial.print("/");
  Serial.print(numDetents);
  Serial.print(" (");
  Serial.print(percent);
  Serial.println("%)");
  notifyStatus();
}

#define HID_KEY_F     0x09
#define HID_KEY_M     0x10
#define HID_KEY_SPACE 0x2C

bool hidReady() {
  return hidInputChar != nullptr && hidConnHandle != 0xFFFF;
}

void sendHidReport(uint8_t key0, uint8_t key1) {
  uint8_t report[8] = {0, 0, key0, key1, 0, 0, 0, 0};
  hidInputChar->setValue(report, 8);
  hidInputChar->notify();
}

void sendHidChordFM() {
  if (!hidReady()) {
    Serial.println("HID: no subscriber, skip F+M");
    return;
  }
  sendHidReport(HID_KEY_F, HID_KEY_M);
  delay(40);
  sendHidReport(0, 0);
  Serial.println("HID: sent F+M");
}

void sendHidDoubleSpace() {
  if (!hidReady()) {
    Serial.println("HID: no subscriber, skip spaces");
    return;
  }
  sendHidReport(HID_KEY_SPACE, 0);
  delay(40);
  sendHidReport(0, 0);
  delay(80);
  sendHidReport(HID_KEY_SPACE, 0);
  delay(40);
  sendHidReport(0, 0);
  Serial.println("HID: sent two Spaces");
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.print("BLE: isConnected = true, handle ");
    Serial.print(connInfo.getConnHandle());
    Serial.print(", count ");
    Serial.println(pServer->getConnectedCount());
    if (pServer->getConnectedCount() < 2) {
      startKnobAdvertising();
    }
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    if (connInfo.getConnHandle() == hidConnHandle) {
      hidConnHandle = 0xFFFF;
    }
    bleConnected = pServer->getConnectedCount() > 0;
    Serial.print("BLE: disconnect handle ");
    Serial.print(connInfo.getConnHandle());
    Serial.print(", reason ");
    Serial.print(reason);
    Serial.print(", remaining ");
    Serial.println(pServer->getConnectedCount());
    startKnobAdvertising();
  }
};

class VolumeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = pChar->getValue();
    if (value.size() >= 1) {
      remapPercent = value.data()[0];
      remapPending = true;
    }
  }
};

class HidInputCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    if (subValue != 0) {
      hidConnHandle = connInfo.getConnHandle();
      Serial.print("BLE: HID subscribed, handle ");
      Serial.println(hidConnHandle);
    } else if (connInfo.getConnHandle() == hidConnHandle) {
      hidConnHandle = 0xFFFF;
      Serial.println("BLE: HID unsubscribed");
    }
  }
};

ServerCallbacks serverCallbacks;
VolumeCallbacks volumeCallbacks;
HidInputCallbacks hidInputCallbacks;

void startKnobAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  NimBLEAdvertisementData advData;
  advData.setName("SmartKnob");
  advData.setAppearance(HID_KEYBOARD);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  NimBLEAdvertisementData scanData;
  scanData.addServiceUUID(SK_SERVICE_UUID);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);

  if (knobServer == nullptr || knobServer->getConnectedCount() < 2) {
    adv->start();
  }
}

void startBle() {
  NimBLEDevice::init("SmartKnob");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityIOCap(0x03); // no input/output → Just Works pairing
  NimBLEDevice::setSecurityAuth(true, false, true); // bond, no MITM, secure conn — iPhone HID

  knobServer = NimBLEDevice::createServer();
  knobServer->setCallbacks(&serverCallbacks);

  NimBLEService* service = knobServer->createService(SK_SERVICE_UUID);

  statusChar = service->createCharacteristic(
    SK_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  NimBLECharacteristic* volumeChar = service->createCharacteristic(
    SK_VOLUME_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  volumeChar->setCallbacks(&volumeCallbacks);

  service->start();

  hidDevice = new NimBLEHIDDevice(knobServer);
  hidDevice->setManufacturer("SmartKnob");
  hidDevice->setPnp(0x02, 0xE502, 0x0001, 0x0110);
  hidDevice->setHidInfo(0x00, 0x01);
  hidDevice->setReportMap(hidReportMap, sizeof(hidReportMap));
  hidDevice->setBatteryLevel(100);
  hidInputChar = hidDevice->getInputReport(1);
  hidInputChar->setCallbacks(&hidInputCallbacks);
  hidDevice->startServices();

  startKnobAdvertising();
}

void setup() {
  Serial.begin(115200);
  delay(200); // USB serial after a reboot, so the next lines actually show up
  Serial.println();
  Serial.print("Reset reason: ");
  Serial.println(resetReasonText(esp_reset_reason()));
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

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
  Serial.println("Send '1', '2', or '3' to switch profiles. Send 'b' for F+M, 's' for two Spaces.");

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
    if (inChar == 'b' || inChar == 'B') sendHidChordFM();
    if (inChar == 's' || inChar == 'S') sendHidDoubleSpace();
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
