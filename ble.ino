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
  // [screen, detent, percent or trim, numDetents]; Menu and Focus only report the screen.
  uint8_t packet[4] = {(uint8_t)uiScreen, 0, 0, 0};
  if (uiScreen == SCREEN_DAVINCI) {
    packet[1] = (uint8_t)lastDetent;
    packet[2] = davinciTrim ? 1 : 0;
  } else if (uiScreen == SCREEN_VOLUME) {
    int level = detentLevel();
    packet[1] = (uint8_t)level;
    packet[2] = percentFromLevel(level);
    packet[3] = (uint8_t)numDetents;
  }
  statusChar->setValue(packet, 4);
  statusChar->notify();
}

void notifyPress() {
  if (pressChar == nullptr || !bleConnected) {
    return;
  }
  uint8_t packet[4];
  encoderPressPacket(packet);
  pressChar->setValue(packet, 4);
  pressChar->notify();
}

void applyVolumeRemap(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  float targetAngle = (percent / 100.0f) * 2.0f * PI;
  // currentAngle = -(sensor.getAngle() - startAngle) = startAngle - sensor.getAngle()
  startAngle = sensor.getAngle() + targetAngle;
  currentAngle = targetAngle;
  lastPcVolume = percent;
  lastDetent = detentLevel();
  detentInitialized = true;
  Serial.print("BLE volume remap ");
  Serial.print(percent);
  Serial.println("%");
  statusNotifyPending = true;
  uiDirty = true;
}

void notifyFocusTrigger(uint8_t value) {
  if (triggerChar == nullptr || !bleConnected) {
    Serial.println("BLE focus skipped (not connected)");
    return;
  }
  triggerChar->setValue(&value, 1);
  triggerChar->notify();
  if (value == TRIGGER_FOCUS_OFF) {
    Serial.println("BLE focus off");
  } else if (value == TRIGGER_PLAY_PAUSE) {
    Serial.println("BLE play/pause");
  } else {
    Serial.println("BLE focus on");
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.println("BLE connect");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = pServer->getConnectedCount() > 0;
    Serial.print("BLE disconnect reason=");
    Serial.println(reason);
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

class PressCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = pChar->getValue();
    if (value.size() >= 1) {
      encoderSetPressDelta(value.data()[0]);
    }
  }
};

ServerCallbacks serverCallbacks;
VolumeCallbacks volumeCallbacks;
PressCallbacks pressCallbacks;

void startKnobAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  NimBLEAdvertisementData advData;
  advData.setName("SmartKnob");
  NimBLEAdvertisementData scanData;
  scanData.addServiceUUID(SK_SERVICE_UUID);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
}

void startBle() {
  NimBLEDevice::init("SmartKnob");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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

  triggerChar = service->createCharacteristic(
    SK_TRIGGER_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  pressChar = service->createCharacteristic(
    SK_PRESS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pressChar->setCallbacks(&pressCallbacks);

  service->start();
  startKnobAdvertising();
}
