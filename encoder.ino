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
