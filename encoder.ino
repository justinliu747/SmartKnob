static const uint16_t AS5048A_READ_ANGLE = 0xFFFF;
static const uint16_t AS5048A_READ_AGC = 0x7FFD;  // DIAAGC, even parity
static const uint16_t AS5048A_READ_ERRFL = 0x4001;
static const SPISettings ENC_SPI_SETTINGS(1000000, MSBFIRST, SPI_MODE1);

#define AGC_PRESS_DELTA 6
#define AGC_RELEASE_DELTA 2
#define AGC_DRIFT 0.005f
#define AGC_READ_MS 10
#define AGC_CAL_READ_MS 2
#define AGC_BINS 128           // raw angle >> 7, 2.8 deg per bin
#define ENC_JUMP_COUNTS 2048   // 1/8 turn
#define ENC_JUMP_MS 5

static uint16_t lastEncRaw = 0;
static uint16_t lastGoodAngle = 0;
static bool haveGoodAngle = false;
static uint8_t lastAgc = 0;
static unsigned long lastAgcReadMs = 0;
static unsigned long lastGoodMs = 0;
static volatile bool encBtnDown = false;

static bool agcCalibrating = false;
static bool agcReady = false;
static uint32_t agcSum[AGC_BINS];
static uint16_t agcCount[AGC_BINS];
static float agcTable[AGC_BINS];
static float agcOffset = 0.0f;
static float agcExpected = 0.0f;
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

static void encoderReadAgc() {
  encoderTransfer(AS5048A_READ_AGC);
  uint16_t diag = encoderTransfer(AS5048A_READ_ANGLE);
  uint16_t data = diag & 0x3FFF;
  lastAgc = data & 0xFF;
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

// Bin i is centered at raw angle i*128 + 64.
static float agcTableAt(uint16_t angle) {
  float pos = ((float)angle - 64.0f) / 128.0f;
  if (pos < 0.0f) {
    pos += AGC_BINS;
  }
  int i = (int)pos;
  float frac = pos - (float)i;
  float a = agcTable[i];
  float b = agcTable[(i + 1) & (AGC_BINS - 1)];
  return a + (b - a) * frac;
}

static void encoderUpdatePress(uint16_t angle) {
  float table = agcTableAt(angle);
  agcExpected = table + agcOffset;
  agcDrop = agcExpected - (float)lastAgc;
  if (agcDrop > agcPeakDrop) {
    agcPeakDrop = agcDrop;
  }

  if (!encBtnDown) {
    if (agcDrop >= AGC_PRESS_DELTA) {
      encBtnDown = true;
      clickHapticPending = true;
    } else {
      agcOffset -= AGC_DRIFT * agcDrop;
    }
  } else if (agcDrop <= AGC_RELEASE_DELTA) {
    encBtnDown = false;
  }
}

bool encoderPressDown() {
  return encBtnDown;
}

void encoderCalStart() {
  for (int i = 0; i < AGC_BINS; i++) {
    agcSum[i] = 0;
    agcCount[i] = 0;
  }
  agcCalibrating = true;
}

void encoderCalFinish() {
  agcCalibrating = false;

  int first = -1;
  int empty = 0;
  for (int i = 0; i < AGC_BINS; i++) {
    if (agcCount[i] > 0) {
      agcTable[i] = (float)agcSum[i] / (float)agcCount[i];
      if (first < 0) {
        first = i;
      }
    } else {
      empty++;
    }
  }

  if (first < 0) {
    for (int i = 0; i < AGC_BINS; i++) {
      agcTable[i] = lastAgc;
    }
  } else {
    for (int k = 1; k < AGC_BINS; k++) {
      int i = (first + k) & (AGC_BINS - 1);
      if (agcCount[i] == 0) {
        agcTable[i] = agcTable[(i - 1) & (AGC_BINS - 1)];
      }
    }
  }

  float lo = agcTable[0];
  float hi = agcTable[0];
  for (int i = 1; i < AGC_BINS; i++) {
    if (agcTable[i] < lo) lo = agcTable[i];
    if (agcTable[i] > hi) hi = agcTable[i];
  }

  agcOffset = 0.0f;
  agcPeakDrop = 0.0f;
  encBtnDown = false;
  agcReady = true;

  Serial.printf("agc cal min=%.1f max=%.1f empty=%d\n", lo, hi, empty);
}

void encoderPrintAgc() {
  Serial.printf("agc=%u exp=%.1f drop=%.1f peak=%.1f down=%d\n",
                lastAgc, agcExpected, agcDrop, agcPeakDrop, encBtnDown ? 1 : 0);
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
      encoderTransfer(AS5048A_READ_ERRFL);
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

  unsigned long agcInterval = agcCalibrating ? AGC_CAL_READ_MS : AGC_READ_MS;
  if (now - lastAgcReadMs >= agcInterval) {
    lastAgcReadMs = now;
    encoderReadAgc();
    if (agcCalibrating) {
      int bin = angle >> 7;
      agcSum[bin] += lastAgc;
      agcCount[bin]++;
    } else if (agcReady) {
      encoderUpdatePress(angle);
    }
  }

  return ((float)angle / 16384.0f) * 2.0f * PI;
}
