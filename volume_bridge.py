"""SmartKnob Windows bridge: BLE status + system volume, simple GUI."""

import asyncio
import threading
import time
import tkinter as tk
from tkinter import ttk

from bleak import BleakClient, BleakScanner
from pycaw.pycaw import AudioUtilities

DEVICE_NAME = "SmartKnob"
SERVICE_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a001"
STATUS_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a002"
VOLUME_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a003"

MODE_NAMES = {1: "Spring", 2: "Detent", 3: "Switch"}


def windows_volume():
    return AudioUtilities.GetSpeakers().EndpointVolume


class KnobApp:
    def __init__(self, root):
        self.root = root
        self.root.title("SmartKnob")
        self.root.geometry("520x420")
        self.root.resizable(True, True)

        self.connected = False
        self.client = None
        self.loop = None
        self.echo_percent = None
        self.ignore_poll_until = 0.0
        self.endpoint = windows_volume()

        self.status_var = tk.StringVar(value="Disconnected")
        self.mode_var = tk.StringVar(value="—")
        self.detent_var = tk.StringVar(value="—")
        self.volume_var = tk.StringVar(value="—")
        self.volume_pct = tk.IntVar(value=0)

        pad = {"padx": 12, "pady": 4}
        ttk.Label(root, text="Connection").grid(row=0, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.status_var).grid(row=0, column=1, sticky="w", **pad)
        ttk.Label(root, text="Mode").grid(row=1, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.mode_var).grid(row=1, column=1, sticky="w", **pad)
        ttk.Label(root, text="Detent").grid(row=2, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.detent_var).grid(row=2, column=1, sticky="w", **pad)
        ttk.Label(root, text="Volume").grid(row=3, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.volume_var).grid(row=3, column=1, sticky="w", **pad)
        ttk.Progressbar(root, maximum=100, variable=self.volume_pct, length=220).grid(
            row=4, column=0, columnspan=2, padx=12, pady=12, sticky="ew"
        )
        ttk.Button(root, text="Reconnect", command=self.ask_reconnect).grid(
            row=5, column=0, columnspan=2, pady=8
        )
        ttk.Label(root, text="BLE devices seen").grid(row=6, column=0, columnspan=2, sticky="w", **pad)
        self.scan_list = tk.Listbox(root, height=10, font=("Consolas", 9))
        self.scan_list.grid(row=7, column=0, columnspan=2, padx=12, pady=(0, 12), sticky="nsew")
        root.grid_rowconfigure(7, weight=1)
        root.grid_columnconfigure(1, weight=1)

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.poll_windows_volume()

    def ui(self, fn):
        self.root.after(0, fn)

    def set_status(self, text):
        self.ui(lambda: self.status_var.set(text))

    def apply_status_packet(self, mode, detent, percent, num_detents, from_knob):
        def update():
            self.mode_var.set(f"{mode} ({MODE_NAMES.get(mode, '?')})")
            self.detent_var.set(f"{detent} / {num_detents}")
            self.volume_var.set(f"{percent}%")
            self.volume_pct.set(percent)

        self.ui(update)

        if from_knob and mode == 2:
            self.echo_percent = percent
            self.ignore_poll_until = time.monotonic() + 0.4
            self.endpoint.SetMasterVolumeLevelScalar(percent / 100.0, None)

    def ask_reconnect(self):
        if self.loop is not None:
            asyncio.run_coroutine_threadsafe(self.reconnect(), self.loop)

    def on_close(self):
        if self.loop is not None:
            asyncio.run_coroutine_threadsafe(self.shutdown(), self.loop)
        self.root.after(200, self.root.destroy)

    def poll_windows_volume(self):
        if self.connected and self.client is not None and time.monotonic() >= self.ignore_poll_until:
            try:
                scalar = self.endpoint.GetMasterVolumeLevelScalar()
                percent = int(round(scalar * 100))
                if self.echo_percent is None or abs(percent - self.echo_percent) > 1:
                    self.echo_percent = percent
                    asyncio.run_coroutine_threadsafe(
                        self.write_percent(percent), self.loop
                    )
            except Exception:
                pass
        self.root.after(250, self.poll_windows_volume)

    async def write_percent(self, percent):
        if self.client is None or not self.client.is_connected:
            return
        await self.client.write_gatt_char(VOLUME_UUID, bytes([percent]), response=False)

    async def on_status(self, _sender, data):
        if len(data) < 4:
            return
        mode, detent, percent, num_detents = data[0], data[1], data[2], data[3]
        self.apply_status_packet(mode, detent, percent, num_detents, from_knob=True)

    def show_scan_results(self, lines):
        def update():
            self.scan_list.delete(0, tk.END)
            if not lines:
                self.scan_list.insert(tk.END, "(no BLE advertisements this scan)")
            else:
                for line in lines:
                    self.scan_list.insert(tk.END, line)

        self.ui(update)

    async def find_knob(self):
        # Bleak 3: discover(return_adv=True) -> {address: (BLEDevice, AdvertisementData)}
        discovered = await BleakScanner.discover(timeout=8.0, return_adv=True)
        match = None
        lines = []
        for address, (device, adv) in discovered.items():
            name = device.name or adv.local_name or "?"
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            line = f"{name}  {address}  uuids={len(uuids)}"
            lines.append(line)
            print("BLE scan:", line, flush=True)
            names = {device.name, adv.local_name}
            if DEVICE_NAME in names or SERVICE_UUID.lower() in uuids:
                match = device
        self.show_scan_results(lines)
        return match, len(discovered)

    async def connect_loop(self):
        while True:
            self.set_status("Scanning…")
            device, seen = await self.find_knob()
            if device is None:
                self.set_status(f"Not found ({seen} BLE ads) — retrying")
                await asyncio.sleep(2)
                continue

            self.set_status(f"Connecting to {device.address}")
            try:
                async with BleakClient(device) as client:
                    self.client = client
                    self.connected = True
                    self.set_status("Connected")
                    await client.start_notify(STATUS_UUID, self.on_status)
                    percent = int(round(self.endpoint.GetMasterVolumeLevelScalar() * 100))
                    self.echo_percent = percent
                    await self.write_percent(percent)
                    while client.is_connected:
                        await asyncio.sleep(0.2)
            except Exception as exc:
                self.set_status(f"Error: {exc}")
            finally:
                self.connected = False
                self.client = None
                self.set_status("Disconnected")
            await asyncio.sleep(1)

    async def reconnect(self):
        if self.client is not None and self.client.is_connected:
            await self.client.disconnect()

    async def shutdown(self):
        if self.client is not None and self.client.is_connected:
            await self.client.disconnect()

    def start_ble_thread(self):
        self.loop = asyncio.new_event_loop()

        def runner():
            asyncio.set_event_loop(self.loop)
            self.loop.create_task(self.connect_loop())
            self.loop.run_forever()

        threading.Thread(target=runner, daemon=True).start()


def main():
    root = tk.Tk()
    app = KnobApp(root)
    app.start_ble_thread()
    root.mainloop()


if __name__ == "__main__":
    main()
