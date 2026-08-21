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
  if (statusChar == nullptr || !bleConnected || uiScreen != SCREEN_VOLUME) {
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
  lastPcVolume = percent;
  lastDetent = detentLevel();
  detentInitialized = true;
  Serial.print("BLE: remapped walls so this pose is ");
  Serial.print(lastDetent);
  Serial.print("/");
  Serial.print(numDetents);
  Serial.print(" (");
  Serial.print(percent);
  Serial.println("%)");
  statusNotifyPending = true;
  uiDirty = true;
}

void notifyFocusTrigger(uint8_t value) {
  if (triggerChar == nullptr || !bleConnected) {
    Serial.println("BLE: focus trigger skipped (not connected)");
    return;
  }
  triggerChar->setValue(&value, 1);
  triggerChar->notify();
  if (value == TRIGGER_FOCUS_OFF) {
    Serial.println("BLE: focus off");
  } else if (value == TRIGGER_PLAY_PAUSE) {
    Serial.println("BLE: play/pause");
  } else {
    Serial.println("BLE: focus trigger");
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.print("BLE: isConnected = true, handle ");
    Serial.println(connInfo.getConnHandle());
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = pServer->getConnectedCount() > 0;
    Serial.print("BLE: disconnect handle ");
    Serial.print(connInfo.getConnHandle());
    Serial.print(", reason ");
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

ServerCallbacks serverCallbacks;
VolumeCallbacks volumeCallbacks;

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

  service->start();
  startKnobAdvertising();
}
