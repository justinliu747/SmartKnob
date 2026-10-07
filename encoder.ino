static const uint16_t AS5048A_READ_ANGLE = 0xFFFF;
static const uint16_t AS5048A_READ_AGC = 0x7FFD;  // DIAAGC, even parity
static const uint16_t AS5048A_READ_ERRFL = 0x4001;
static const SPISettings ENC_SPI_SETTINGS(4000000, MSBFIRST, SPI_MODE1);

// AGC moves in steps of ~3 and slews ~3 counts per 10-20 ms; a grip while turning drops 3-6.
#define AGC_PRESS_DELTA 12
#define AGC_RELEASE_DELTA 6
#define AGC_TRACK_BAND 3       // rest only follows readings within one AGC step
#define AGC_DRIFT 0.005f
#define AGC_MIN_HOLD_MS 80     // covers the click kick and flexure bounce
#define AGC_SETTLE_SAMPLES 20
#define AGC_READ_MS 10
#define ENC_JUMP_COUNTS 2048   // 1/8 turn
#define ENC_JUMP_MS 5

static uint16_t lastEncRaw = 0;
static uint16_t lastGoodAngle = 0;
static bool haveGoodAngle = false;
static uint8_t lastAgc = 0;
static unsigned long lastAgcReadMs = 0;
static unsigned long lastGoodMs = 0;
static volatile bool encBtnDown = false;
static unsigned long pressStartMs = 0;
static uint8_t agcSettle = 0;

static bool agcReady = false;
static float agcRest = 0.0f;
static float agcDrop = 0.0f;
static float agcPeakDrop = 0.0f;

static uint16_t encoderTransfer(uint16_t cmd) {
  encoderSPI.beginTransaction(ENC_SPI_SETTINGS);
  digitalWrite(ENC_CS, LOW);
  delayMicroseconds(1);
  uint16_t val = encoderSPI.transfer16(cmd);
  digitalWrite(ENC_CS, HIGH);
  encoderSPI.endTransaction();
  return val;
}

static bool encoderFrameOk(uint16_t raw) {
  if (raw & 0x4000) {
    return false;
  }
  uint16_t x = raw & 0x7FFF;
  x ^= x >> 8;
  x ^= x >> 4;
  x ^= x >> 2;
  x ^= x >> 1;
  return ((x & 1) == (raw >> 15));
}

// ERRFL contents come back on the next frame, so flush it with a throwaway read.
static void encoderClearError() {
  encoderTransfer(AS5048A_READ_ERRFL);
  encoderTransfer(AS5048A_READ_ANGLE);
}

static bool encoderReadAgc() {
  encoderTransfer(AS5048A_READ_AGC);
  uint16_t diag = encoderTransfer(AS5048A_READ_ANGLE);
  if (!encoderFrameOk(diag)) {
    if (diag & 0x4000) {
      encoderClearError();
    }
    return false;
  }
  lastAgc = diag & 0xFF;
  return true;
}

static uint16_t encoderAngleDelta(uint16_t a, uint16_t b) {
  int16_t d = (int16_t)(a - b);
  if (d > 8192) {
    d -= 16384;
  } else if (d < -8192) {
    d += 16384;
  }
  if (d < 0) {
    d = -d;
  }
  return (uint16_t)d;
}

// Readings above rest (field weaker, knob lifted) are never a press and never move rest.
static void encoderUpdatePress(unsigned long now) {
  if (agcSettle > 0) {
    agcSettle--;
    agcRest += 0.2f * ((float)lastAgc - agcRest);
    if (agcSettle == 0) {
      Serial.printf("agc rest=%.1f\n", agcRest);
    }
    return;
  }

  agcDrop = agcRest - (float)lastAgc;
  if (agcDrop > agcPeakDrop) {
    agcPeakDrop = agcDrop;
  }

  if (!encBtnDown) {
    if (agcDrop >= AGC_PRESS_DELTA) {
      encBtnDown = true;
      pressStartMs = now;
      clickHapticPending = true;
    } else if (fabsf(agcDrop) <= AGC_TRACK_BAND) {
      agcRest -= AGC_DRIFT * agcDrop;
    }
  } else if (agcDrop <= AGC_RELEASE_DELTA && (now - pressStartMs) >= AGC_MIN_HOLD_MS) {
    encBtnDown = false;
  }
}

bool encoderPressDown() {
  return encBtnDown;
}

// Call once the motor is aligned; rest is learned from the next samples, hands off.
void encoderStartPress() {
  agcRest = lastAgc;
  agcPeakDrop = 0.0f;
  encBtnDown = false;
  agcSettle = AGC_SETTLE_SAMPLES;
  agcReady = true;
}

void encoderPrintAgc() {
  Serial.printf("agc=%u rest=%.1f drop=%.1f peak=%.1f down=%d loop_us=%u\n",
                lastAgc, agcRest, agcDrop, agcPeakDrop, encBtnDown ? 1 : 0,
                (unsigned)motor.loopfoc_time_us);
  agcPeakDrop = agcDrop;
}

void encoderInit() {
  pinMode(ENC_CS, OUTPUT);
  digitalWrite(ENC_CS, HIGH);
  encoderSPI.begin(ENC_SCLK, ENC_MISO, ENC_MOSI, ENC_CS);

  // First frame is discarded; AS5048A returns the previous command on MISO.
  encoderTransfer(AS5048A_READ_ANGLE);
  delayMicroseconds(10);
  lastEncRaw = encoderTransfer(AS5048A_READ_ANGLE);
  if (encoderFrameOk(lastEncRaw)) {
    lastGoodAngle = lastEncRaw & 0x3FFF;
    haveGoodAngle = true;
    lastGoodMs = millis();
  }
  encoderReadAgc();
}

float encoderGetAngle() {
  uint16_t raw = encoderTransfer(AS5048A_READ_ANGLE);
  if (!encoderFrameOk(raw)) {
    if (raw & 0x4000) {
      encoderClearError();
    }
    return -1.0f;
  }

  uint16_t angle = raw & 0x3FFF;
  unsigned long now = millis();
  if (haveGoodAngle && encoderAngleDelta(angle, lastGoodAngle) >= ENC_JUMP_COUNTS &&
      (now - lastGoodMs) < ENC_JUMP_MS) {
    return -1.0f;
  }

  lastEncRaw = raw;
  lastGoodAngle = angle;
  lastGoodMs = now;
  haveGoodAngle = true;

  if (now - lastAgcReadMs >= AGC_READ_MS) {
    lastAgcReadMs = now;
    if (encoderReadAgc() && agcReady) {
      encoderUpdatePress(now);
    }
  }

  return ((float)angle / 16384.0f) * 2.0f * PI;
}
