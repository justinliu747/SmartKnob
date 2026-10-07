static const uint16_t AS5048A_READ_ANGLE = 0xFFFF;
static const uint16_t AS5048A_READ_AGC = 0x7FFD;  // DIAAGC, even parity
static const uint16_t AS5048A_READ_ERRFL = 0x4001;
static const SPISettings ENC_SPI_SETTINGS(4000000, MSBFIRST, SPI_MODE1);

// AGC moves in steps of ~3 and slews ~3 counts per 10-20 ms; a grip while turning drops 3-6.
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
// Set live from the bridge over BLE. Release at half the press point keeps hysteresis (12 -> 6).
static volatile uint8_t agcPressDelta = 12;
static unsigned long pressStartMs = 0;
static uint8_t agcSettle = 0;

static bool agcReady = false;
static float agcRest = 0.0f;
static float agcDrop = 0.0f;
static float agcPeakDrop = 0.0f;

#define ENC_FLAG_BAD_FRAME 1
#define ENC_FLAG_JUMP      2
#define ENC_FLAG_AGC_READ  4
static volatile uint8_t encFlags = 0;

// Filled on core 1 every AGC sample, drained by uiTask on core 0.
#define AGC_LOG_SIZE 128
struct AgcLogEntry {
  uint32_t tMs;
  uint16_t raw;
  uint8_t agc;
  uint8_t down;
  uint8_t kick;
  uint8_t screen;
  int16_t uqMv;
  float rest;
};
static AgcLogEntry agcLog[AGC_LOG_SIZE];
static volatile uint16_t agcLogHead = 0;
static uint16_t agcLogTail = 0;

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
    if (agcDrop >= agcPressDelta) {
      encBtnDown = true;
      pressStartMs = now;
      clickHapticPending = true;
    } else if (fabsf(agcDrop) <= AGC_TRACK_BAND) {
      agcRest -= AGC_DRIFT * agcDrop;
    }
  } else if (agcDrop <= agcPressDelta / 2 && (now - pressStartMs) >= AGC_MIN_HOLD_MS) {
    encBtnDown = false;
  }
}

void encoderSetPressDelta(uint8_t d) {
  agcPressDelta = _constrain(d, 3, 60);
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

uint16_t encoderRawAngle() {
  return lastGoodAngle;
}

uint8_t encoderTakeFlags() {
  uint8_t f = encFlags;
  encFlags = 0;
  return f;
}

// A,t_ms,raw,agc,rest,drop,down,kick,screen,uq
void encoderDrainLog() {
  while (agcLogTail != agcLogHead) {
    const AgcLogEntry& e = agcLog[agcLogTail];
    Serial.printf("A,%lu,%u,%u,%.2f,%.2f,%u,%u,%u,%.3f\n",
                  (unsigned long)e.tMs, e.raw, e.agc, e.rest, e.rest - (float)e.agc,
                  e.down, e.kick, e.screen, e.uqMv / 1000.0f);
    agcLogTail = (agcLogTail + 1) % AGC_LOG_SIZE;
  }
}

void encoderPrintAgc() {
  Serial.printf("agc=%u rest=%.1f drop=%.1f peak=%.1f down=%d loop_us=%u\n",
                lastAgc, agcRest, agcDrop, agcPeakDrop, encBtnDown ? 1 : 0,
                (unsigned)motor.loopfoc_time_us);
  agcPeakDrop = agcDrop;
}

// [drop*4 as int8, down, press delta, release delta]
void encoderPressPacket(uint8_t* p) {
  float q = _constrain(agcDrop * 4.0f, -128.0f, 127.0f);
  p[0] = (uint8_t)(int8_t)lroundf(q);
  p[1] = encBtnDown ? 1 : 0;
  p[2] = agcPressDelta;
  p[3] = agcPressDelta / 2;
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
    encFlags |= ENC_FLAG_BAD_FRAME;
    if (raw & 0x4000) {
      encoderClearError();
    }
    return -1.0f;
  }

  uint16_t angle = raw & 0x3FFF;
  unsigned long now = millis();
  if (haveGoodAngle && encoderAngleDelta(angle, lastGoodAngle) >= ENC_JUMP_COUNTS &&
      (now - lastGoodMs) < ENC_JUMP_MS) {
    encFlags |= ENC_FLAG_JUMP;
    return -1.0f;
  }

  lastEncRaw = raw;
  lastGoodAngle = angle;
  lastGoodMs = now;
  haveGoodAngle = true;

  if (now - lastAgcReadMs >= AGC_READ_MS) {
    lastAgcReadMs = now;
    encFlags |= ENC_FLAG_AGC_READ;
    if (encoderReadAgc()) {
      if (agcReady) {
        encoderUpdatePress(now);
        if (agcLogOn) {
          uint16_t next = (agcLogHead + 1) % AGC_LOG_SIZE;
          if (next != agcLogTail) {
            AgcLogEntry& e = agcLog[agcLogHead];
            e.tMs = now;
            e.raw = angle;
            e.agc = lastAgc;
            e.down = encBtnDown ? 1 : 0;
            e.kick = clickRunning ? 1 : 0;
            e.screen = (uint8_t)uiScreen;
            e.uqMv = (int16_t)(motor.voltage.q * 1000.0f);
            e.rest = agcRest;
            agcLogHead = next;
          }
        }
      }
    }
  }

  return ((float)angle / 16384.0f) * 2.0f * PI;
}
