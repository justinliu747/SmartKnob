void drawProgressRing(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }

  const int cx = 120;
  const int cy = 120;
  const int r = 108;
  const int ir = 92;
  const uint16_t track = 0x2104;
  const uint16_t fill = 0x07FF;

  spr.drawArc(cx, cy, r, ir, 0, 360, track, TFT_BLACK, true);
  if (percent <= 0) {
    return;
  }

  // TFT_eSPI: 0° is 6 o'clock, clockwise. 180° is 12 o'clock.
  int startA = 180;
  int endA = startA + (percent * 360) / 100;
  if (endA <= 360) {
    spr.drawArc(cx, cy, r, ir, startA, endA, fill, TFT_BLACK, true);
  } else {
    spr.drawArc(cx, cy, r, ir, startA, 360, fill, TFT_BLACK, true);
    spr.drawArc(cx, cy, r, ir, 0, endA - 360, fill, TFT_BLACK, true);
  }
}

void drawMenu() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);

  const char* labels[MENU_COUNT] = {"Volume", "Focus", "Davinci"};
  const int ys[MENU_COUNT] = {88, 120, 152};
  for (int i = 0; i < MENU_COUNT; i++) {
    if (i == menuIndex) {
      spr.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
      spr.setTextColor(0x8410, TFT_BLACK);
    }
    spr.drawString(labels[i], 120, ys[i]);
  }
}

void drawVolume() {
  spr.fillSprite(TFT_BLACK);
  uint8_t percent = percentFromLevel(detentLevel());
  drawProgressRing(percent);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(buf, 120, 120);
}

void drawFocusClock(int minutes, int seconds) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(buf, 120, 120);
}

void drawFocus() {
  spr.fillSprite(TFT_BLACK);
  if (focusPhase == FOCUS_RUNNING) {
    unsigned long remaining = focusRemainingMs();
    int remSec = (int)(remaining / 1000UL);
    int pct = 0;
    if (focusTargetMs > 0) {
      pct = (int)((remaining * 100UL) / focusTargetMs);
    }
    drawProgressRing(pct);
    drawFocusClock(remSec / 60, remSec % 60);
  } else {
    drawFocusClock(focusMinutes, 0);
  }
}

void drawFocusOverlay() {
  spr.fillRect(18, 72, 204, 96, 0x1082);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(TFT_WHITE, 0x1082);
  spr.drawString("Focus session done", 120, 98);

  char buf[32];
  snprintf(buf, sizeof(buf), "Locked in for %d mins", focusElapsedMinutes);
  spr.drawString(buf, 120, 122);
}

void drawDavinci() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);

  const char* labels[2] = {"Cut Jump", "Trim"};
  const int ys[2] = {104, 136};
  int active = davinciTrim ? 1 : 0;
  for (int i = 0; i < 2; i++) {
    if (i == active) {
      spr.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
      spr.setTextColor(0x8410, TFT_BLACK);
    }
    spr.drawString(labels[i], 120, ys[i]);
  }
}

void updateUi() {
  if (!uiDirty || !spriteOk) {
    return;
  }
  uiDirty = false;

  if (uiScreen == SCREEN_MENU) {
    drawMenu();
  } else if (uiScreen == SCREEN_VOLUME) {
    drawVolume();
  } else if (uiScreen == SCREEN_DAVINCI) {
    drawDavinci();
  } else {
    drawFocus();
  }

  if (focusPhase == FOCUS_DONE) {
    drawFocusOverlay();
  }

  spr.pushSprite(0, 0);
}
