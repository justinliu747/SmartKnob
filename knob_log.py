"""Guided SmartKnob debug capture. Close the Arduino serial monitor, then:

    python knob_log.py          (auto-finds the ESP32-S3)
    python knob_log.py COM3

Everything the knob prints is saved to logs/knob_<time>.txt.
"""

import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    raise SystemExit("Knob not found. Pass the port, e.g. python knob_log.py COM3")


class Capture:
    def __init__(self, port, path):
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.out = open(path, "w", encoding="utf-8")
        self.rumble_done = threading.Event()
        self.running = True
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while self.running:
            buf += self.ser.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.out.write(line + "\n")
                if line.startswith("R_END"):
                    self.rumble_done.set()
                if not (line.startswith("R,") or line.startswith("A,") or line.startswith("CAL,")):
                    print("  knob> " + line)
            self.out.flush()

    def send(self, cmd):
        self.ser.write(cmd.encode())

    def note(self, text):
        self.out.write(f"NOTE {text}\n")
        self.out.flush()

    def rumble(self, label):
        self.note(f"rumble {label}")
        self.rumble_done.clear()
        self.send("r")
        if not self.rumble_done.wait(30):
            print("  (no rumble dump received)")
        else:
            print("  recorded.")

    def close(self):
        self.running = False
        time.sleep(0.3)
        self.ser.close()
        self.out.close()


def step(text):
    input(f"\n{text}\n  Press Enter when ready... ")


def ask(cap, key, text):
    answer = input(f"\n{text}\n  > ").strip()
    cap.note(f"{key}={answer}")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    Path("logs").mkdir(exist_ok=True)
    path = Path("logs") / f"knob_{datetime.now():%Y%m%d_%H%M%S}.txt"
    cap = Capture(port, path)
    print(f"Logging {port} -> {path}")

    try:
        step("1. Press RESET on the board. Keep hands OFF the knob until the calibration "
             "spin is done and the menu is on screen.")
        cap.note("boot done")

        print("\n--- Rumble (each recording is 1 second) ---")
        step("2. Menu screen, hands OFF the knob.")
        cap.rumble("menu idle hands off")

        step("3. Rest your fingers on the knob without turning it.")
        cap.rumble("menu idle fingers resting")

        step("4. Turn slowly and steadily (about 1 turn per 3 s). Press Enter WHILE turning, "
             "keep turning until it says recorded.")
        cap.rumble("menu slow turn")

        step("5. Same, but turn fast.")
        cap.rumble("menu fast turn")

        cap.send("z")
        step("6. Motor torque is now OFF (no detents). Turn slowly, press Enter WHILE turning.")
        cap.rumble("torque off slow turn")
        ask(cap, "rumble_torque_off", "Did you still feel the rumble with the motor off? (yes / no / unsure)")
        cap.send("z")
        ask(cap, "rumble_where", "In steps 2-5, where did you feel the rumble? (e.g. idle, slow, fast, all)")

        print("\n--- AGC / press ---")
        cap.send("l")
        step("7. Menu screen. Slowly turn through the detents for about 10 s, do NOT press. "
             "Press Enter when done.")
        cap.note("agc menu turning done")

        step("8. Hold the knob halfway between two menu detents (fighting the motor) for about "
             "3 s, then let go. Press Enter when done.")
        cap.note("agc hold against detent done")

        step("9. Go to the Volume screen. Turn slowly for about 10 s, do NOT press. "
             "Press Enter when done.")
        cap.note("agc volume turning done")

        step("10. On Volume, press the knob 5 times, normal clicks, about 1 s apart "
             "(this toggles play/pause). Press Enter when done.")
        cap.note("agc volume presses done")
        ask(cap, "presses_felt", "Out of 5, how many haptic clicks did you feel?")
        ask(cap, "presses_registered", "Out of 5, how many toggled play/pause?")

        step("11. Long-press back to the Menu. Turn and press at the same time, 3 times. "
             "Press Enter when done.")
        cap.note("agc press while turning done")
        cap.send("l")

        ask(cap, "other", "Anything else you noticed? (Enter to skip)")
    finally:
        cap.close()
    print(f"\nDone. Saved {path}")


if __name__ == "__main__":
    main()
