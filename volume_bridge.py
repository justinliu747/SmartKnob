"""SmartKnob Windows bridge: BLE status + system volume, simple GUI."""

import ctypes
import os
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path

DEVICE_NAME = "SmartKnob"
SERVICE_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a001"
STATUS_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a002"
VOLUME_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a003"
TRIGGER_UUID = "cba1d411-0e8f-4e5c-8a21-6f3c9b01a004"

MODE_NAMES = {1: "Spring", 2: "Detent", 3: "Switch", 4: "Davinci"}
ENV_PATH = Path(__file__).resolve().parent / ".env"
FOCUS_ON_SUBJECT = "focus trigger"
FOCUS_OFF_SUBJECT = "focus off"
FOCUS_BODY = "sent from smartknob"
TRIGGER_ON = 1
TRIGGER_OFF = 2
TRIGGER_PLAY_PAUSE = 3
VK_MEDIA_PLAY_PAUSE = 0xB3
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
ICLOUD_SMTP = "smtp.mail.icloud.com"
ICLOUD_SMTP_PORT = 587
RESOLVE_MODULES = (
    r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules"
)
FUSION_DLL = r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll"
CREATE_NO_WINDOW = 0x08000000
INPUT_KEYBOARD = 1
ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

_resolve_python = None
_resolve_app = None
_resolve_worker = None
_resolve_job_lock = threading.Lock()
_cache_key = None
_cache_stops = None
_cache_offset = None
_cache_at = 0.0
STOP_CACHE_TTL = 0.5


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", ctypes.c_ushort),
        ("wScan", ctypes.c_ushort),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long),
        ("dy", ctypes.c_long),
        ("mouseData", ctypes.c_ulong),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", ctypes.c_ulong),
        ("wParamL", ctypes.c_ushort),
        ("wParamH", ctypes.c_ushort),
    ]


class INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("union", INPUTUNION)]


def log(msg):
    print(msg, flush=True)


def fps_int(project):
    raw = str(project.GetSetting("timelineFrameRate") or "24")
    raw = raw.replace("DF", "").replace("df", "").strip().split()[0]
    try:
        return max(1, int(round(float(raw))))
    except ValueError:
        return 24


def tc_to_frame(tc, fps):
    parts = str(tc).replace(";", ":").split(":")
    if len(parts) != 4:
        return 0
    try:
        h, m, s, f = (int(p) for p in parts)
    except ValueError:
        return 0
    return ((h * 60 + m) * 60 + s) * fps + f


def frame_to_tc(frame, fps):
    if frame < 0:
        frame = 0
    f = int(frame) % fps
    total_s = int(frame) // fps
    s = total_s % 60
    total_m = total_s // 60
    m = total_m % 60
    h = total_m // 60
    return f"{h:02d}:{m:02d}:{s:02d}:{f:02d}"


def detent_delta(prev, curr):
    d = (curr - prev) % 256
    if d > 127:
        d -= 256
    return d


def clip_stops(timeline):
    items = timeline.GetItemListInTrack("video", 1) or []
    if not items:
        return []
    items = sorted(items, key=lambda it: int(it.GetStart()))
    stops = []
    for item in items:
        start = int(item.GetStart())
        last = int(item.GetEnd()) - 1
        stops.append(start)
        if last > start:
            stops.append(last)
    return sorted(set(stops))


def directional_stop_index(frame, stops, delta):
    if delta > 0:
        idx = None
        for i, stop in enumerate(stops):
            if stop > frame:
                idx = i
                break
        if idx is None:
            return len(stops) - 1
        idx += delta - 1
    elif delta < 0:
        idx = None
        for i in range(len(stops) - 1, -1, -1):
            if stops[i] < frame:
                idx = i
                break
        if idx is None:
            return 0
        idx += delta + 1
    else:
        return 0
    if idx < 0:
        return 0
    if idx >= len(stops):
        return len(stops) - 1
    return idx


def cached_stops(project, timeline, fps):
    global _cache_key, _cache_stops, _cache_offset, _cache_at
    key = (project.GetName(), timeline.GetName(), fps)
    now = time.monotonic()
    if _cache_key != key or _cache_stops is None or now - _cache_at > STOP_CACHE_TTL:
        offset = timeline_frame_offset(timeline, fps)
        _cache_key = key
        _cache_offset = offset
        _cache_stops = [stop + offset for stop in clip_stops(timeline)]
        _cache_at = now
    return _cache_offset, _cache_stops


def timeline_frame_offset(timeline, fps):
    return tc_to_frame(timeline.GetStartTimecode(), fps) - int(timeline.GetStartFrame())


def foreground_exe():
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    hwnd = user32.GetForegroundWindow()
    pid = ctypes.c_ulong(0)
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if not pid.value:
        return ""
    handle = kernel32.OpenProcess(0x1000, False, pid.value)
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_ulong(32768)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return buf.value
    finally:
        kernel32.CloseHandle(handle)
    return ""


def resolve_is_focused():
    path = foreground_exe().replace("/", "\\").lower()
    return path.endswith("\\resolve.exe")


def send_vk(vk, up=False):
    user32 = ctypes.windll.user32
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = 0x0008
    if up:
        flags |= KEYEVENTF_KEYUP
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki = KEYBDINPUT(0, scan, flags, 0, 0)
    sent = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if sent != 1:
        raise RuntimeError("SendInput failed")


def send_nudge(delta):
    n = abs(int(delta))
    if n > 30:
        n = 30
    vk = 0xBE if delta > 0 else 0xBC
    for _ in range(n):
        send_vk(vk, False)
        send_vk(vk, True)
    log(f"Resolve: nudge {delta:+d}")


def connect_resolve():
    global _resolve_app
    if _resolve_app is None:
        if RESOLVE_MODULES not in sys.path:
            sys.path.append(RESOLVE_MODULES)
        os.environ.setdefault("RESOLVE_SCRIPT_LIB", FUSION_DLL)
        try:
            import DaVinciResolveScript as dvr
        except ImportError as exc:
            raise RuntimeError(f"Resolve module missing: {exc}") from exc
        resolve = dvr.scriptapp("Resolve")
        if resolve is None:
            raise RuntimeError("Resolve not running (enable External scripting: Local)")
        _resolve_app = resolve
    resolve = _resolve_app
    page = resolve.GetCurrentPage()
    if page != "edit":
        log(f"Resolve: idle (page={page})")
        return None
    project = resolve.GetProjectManager().GetCurrentProject()
    if project is None:
        raise RuntimeError("No Resolve project open")
    timeline = project.GetCurrentTimeline()
    if timeline is None:
        raise RuntimeError("No timeline open")
    return resolve, project, timeline


def step_resolve_playhead(delta):
    ctx = connect_resolve()
    if ctx is None:
        return
    _resolve, project, timeline = ctx
    fps = fps_int(project)
    _offset, stops = cached_stops(project, timeline, fps)
    if not stops:
        raise RuntimeError("No clips on video track 1")
    frame = tc_to_frame(timeline.GetCurrentTimecode(), fps)
    idx = directional_stop_index(frame, stops, delta)
    if not timeline.SetCurrentTimecode(frame_to_tc(stops[idx], fps)):
        raise RuntimeError("SetCurrentTimecode failed")
    log(f"Resolve: playhead -> {frame_to_tc(stops[idx], fps)}")


def find_resolve_python():
    global _resolve_python
    if _resolve_python:
        return _resolve_python
    if sys.version_info < (3, 13):
        _resolve_python = sys.executable
        return _resolve_python
    for ver in ("3.12", "3.11", "3.10"):
        try:
            out = subprocess.check_output(
                ["py", f"-{ver}", "-c", "import sys; print(sys.executable)"],
                text=True,
                timeout=8,
            )
            exe = out.strip()
            if exe:
                log(f"Resolve: using {exe} (fusionscript.dll crashes on Python 3.13)")
                _resolve_python = exe
                return exe
        except Exception:
            continue
    raise RuntimeError(
        "Resolve needs Python 3.10-3.12. This process is 3.13, which crashes fusionscript.dll."
    )


def ensure_resolve_worker():
    global _resolve_worker
    if _resolve_worker is not None and _resolve_worker.poll() is None:
        return _resolve_worker
    py = find_resolve_python()
    env = os.environ.copy()
    env["RESOLVE_SCRIPT_LIB"] = FUSION_DLL
    env["PYTHONPATH"] = RESOLVE_MODULES
    flags = CREATE_NO_WINDOW if os.name == "nt" else 0
    _resolve_worker = subprocess.Popen(
        [py, "-u", str(Path(__file__).resolve()), "--resolve-job"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        bufsize=1,
        creationflags=flags,
    )
    log(f"Resolve: worker pid={_resolve_worker.pid}")
    return _resolve_worker


def stop_resolve_worker():
    global _resolve_worker
    worker = _resolve_worker
    _resolve_worker = None
    if worker is None:
        return
    try:
        if worker.stdin:
            worker.stdin.close()
        worker.terminate()
        worker.wait(timeout=2)
    except Exception:
        try:
            worker.kill()
        except Exception:
            pass


def run_resolve_job(*args):
    if not resolve_is_focused():
        log("Resolve: idle (not focused)")
        return "Resolve: idle (not focused)"
    log(f"Resolve: job {' '.join(args)}")
    with _resolve_job_lock:
        worker = ensure_resolve_worker()
        try:
            worker.stdin.write(" ".join(args) + "\n")
            worker.stdin.flush()
        except Exception as exc:
            stop_resolve_worker()
            raise RuntimeError(f"Resolve worker write failed: {exc}") from exc
        lines = []
        while True:
            line = worker.stdout.readline()
            if not line:
                stop_resolve_worker()
                raise RuntimeError("Resolve worker died")
            line = line.rstrip("\r\n")
            if line == "END":
                break
            log(line)
            lines.append(line)
        return "\n".join(lines)


def resolve_job_main(argv):
    try:
        if argv[:1] == ["arm"]:
            connect_resolve()
            log("OK")
            return 0
        if argv[:1] == ["step"] and len(argv) >= 2:
            step_resolve_playhead(int(argv[1]))
            log("OK")
            return 0
        log("Resolve: unknown job " + " ".join(argv))
        return 2
    except Exception as exc:
        log(f"Resolve: {exc}")
        traceback.print_exc()
        return 1


def resolve_job_loop():
    for raw in sys.stdin:
        argv = raw.split()
        if argv:
            resolve_job_main(argv)
        log("END")


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--resolve-job":
    if len(sys.argv) == 2:
        resolve_job_loop()
        sys.exit(0)
    sys.exit(resolve_job_main(sys.argv[2:]))


import asyncio
import ctypes
import smtplib
import threading
import tkinter as tk
from email.message import EmailMessage
from tkinter import ttk

from bleak import BleakClient, BleakScanner
from pycaw.pycaw import AudioUtilities


def windows_volume():
    return AudioUtilities.GetSpeakers().EndpointVolume


def load_env(path):
    values = {}
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return values
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def send_play_pause():
    user32 = ctypes.windll.user32
    user32.keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_EXTENDEDKEY, 0)
    user32.keybd_event(
        VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0
    )


def send_focus_email(subject):
    env = load_env(ENV_PATH)
    address = env.get("ICLOUD_EMAIL", "").strip()
    password = env.get("ICLOUD_APP_PASSWORD", "").replace(" ", "")
    if not address or not password:
        raise RuntimeError("Set ICLOUD_EMAIL and ICLOUD_APP_PASSWORD in .env")
    msg = EmailMessage()
    msg["From"] = address
    msg["To"] = address
    msg["Subject"] = subject
    msg.set_content(FOCUS_BODY)
    with smtplib.SMTP(ICLOUD_SMTP, ICLOUD_SMTP_PORT, timeout=20) as smtp:
        smtp.starttls()
        smtp.login(address, password)
        smtp.send_message(msg)


class KnobApp:
    def __init__(self, root):
        self.root = root
        self.root.title("SmartKnob")
        self.root.geometry("520x440")
        self.root.resizable(True, True)

        self.connected = False
        self.client = None
        self.loop = None
        self.echo_percent = None
        self.ignore_poll_until = 0.0
        self.endpoint = windows_volume()
        self.knob_mode = None
        self.resolve_armed = False
        self.resolve_last_detent = None
        self.davinci_trim = False
        self.trim_ignore_packet = False
        self.step_pending = 0
        self.step_busy = False
        self.step_lock = threading.Lock()

        self.status_var = tk.StringVar(value="Disconnected")
        self.mode_var = tk.StringVar(value="—")
        self.detent_var = tk.StringVar(value="—")
        self.volume_var = tk.StringVar(value="—")
        self.volume_pct = tk.IntVar(value=0)
        self.email_var = tk.StringVar(value="—")
        self.resolve_var = tk.StringVar(value="—")

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
        ttk.Label(root, text="Email").grid(row=5, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.email_var).grid(row=5, column=1, sticky="w", **pad)
        ttk.Label(root, text="Resolve").grid(row=6, column=0, sticky="w", **pad)
        ttk.Label(root, textvariable=self.resolve_var).grid(row=6, column=1, sticky="w", **pad)
        ttk.Button(root, text="Reconnect", command=self.ask_reconnect).grid(
            row=7, column=0, columnspan=2, pady=8
        )
        ttk.Label(root, text="BLE devices seen").grid(row=8, column=0, columnspan=2, sticky="w", **pad)
        self.scan_list = tk.Listbox(root, height=8, font=("Consolas", 9))
        self.scan_list.grid(row=9, column=0, columnspan=2, padx=12, pady=(0, 12), sticky="nsew")
        root.grid_rowconfigure(9, weight=1)
        root.grid_columnconfigure(1, weight=1)

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.poll_windows_volume()

    def ui(self, fn):
        self.root.after(0, fn)

    def set_status(self, text):
        self.ui(lambda: self.status_var.set(text))

    def set_email_status(self, text):
        self.ui(lambda: self.email_var.set(text))

    def set_resolve_status(self, text):
        self.ui(lambda: self.resolve_var.set(text))

    def apply_status_packet(self, mode, detent, percent, num_detents, from_knob):
        def update():
            self.mode_var.set(f"{mode} ({MODE_NAMES.get(mode, '?')})")
            self.detent_var.set(f"{detent} / {num_detents}")
            self.volume_var.set(f"{percent}%")
            self.volume_pct.set(percent)

        self.ui(update)
        self.knob_mode = mode

        if not from_knob:
            return

        if mode == 2:
            self.resolve_armed = False
            self.echo_percent = percent
            self.ignore_poll_until = time.monotonic() + 0.4
            self.endpoint.SetMasterVolumeLevelScalar(percent / 100.0, None)
            return

        if mode == 4:
            trim = percent != 0
            log(f"BLE: davinci detent={detent} trim={int(trim)}")
            detent_copy = detent
            trim_copy = trim
            self.ui(lambda: self.handle_davinci_detent(detent_copy, trim_copy))
            return

        self.resolve_armed = False

    def handle_davinci_detent(self, detent, trim):
        if trim != self.davinci_trim:
            self.davinci_trim = trim
            self.resolve_last_detent = detent
            self.trim_ignore_packet = True
            log(f"Resolve: mode {'Trim' if trim else 'Cut Jump'}")
            self.set_resolve_status("Trim" if trim else "Cut Jump")
            return

        if self.trim_ignore_packet:
            self.resolve_last_detent = detent
            self.trim_ignore_packet = False
            log(f"Resolve: ignore detent={detent}")
            return

        if self.davinci_trim:
            if self.resolve_last_detent is None:
                self.resolve_last_detent = detent
                return
            delta = detent_delta(self.resolve_last_detent, detent)
            self.resolve_last_detent = detent
            if delta == 0:
                return
            if not resolve_is_focused():
                log("Resolve: idle (not focused)")
                self.set_resolve_status("Idle")
                return
            send_nudge(delta)
            self.set_resolve_status("Trim")
            return

        if not self.resolve_armed:
            log(f"Resolve: arm detent={detent}")
            try:
                out = run_resolve_job("arm")
            except Exception as exc:
                log(f"Resolve: connect failed: {exc}")
                traceback.print_exc()
                self.set_resolve_status(str(exc))
                return
            if "idle" in out:
                self.set_resolve_status("Idle")
                return
            self.resolve_armed = True
            self.resolve_last_detent = detent
            self.set_resolve_status("Cut Jump")
            log("Resolve: armed")
            return

        delta = detent_delta(self.resolve_last_detent, detent)
        self.resolve_last_detent = detent
        if delta == 0:
            return
        log(f"Resolve: step {delta:+d} (detent={detent})")
        self.enqueue_cut_step(delta)

    def enqueue_cut_step(self, delta):
        with self.step_lock:
            self.step_pending += delta
            if self.step_busy:
                return
            self.step_busy = True
        threading.Thread(target=self.cut_step_worker, daemon=True).start()

    def cut_step_worker(self):
        while True:
            with self.step_lock:
                delta = self.step_pending
                self.step_pending = 0
                if delta == 0:
                    self.step_busy = False
                    return
            try:
                out = run_resolve_job("step", str(delta))
                self.set_resolve_status("Idle" if "idle" in out else "Cut Jump")
            except Exception as exc:
                log(f"Resolve: step failed: {exc}")
                traceback.print_exc()
                self.set_resolve_status(str(exc))
                with self.step_lock:
                    self.step_busy = False
                    return

    def ask_reconnect(self):
        if self.loop is not None:
            asyncio.run_coroutine_threadsafe(self.reconnect(), self.loop)

    def on_close(self):
        stop_resolve_worker()
        if self.loop is not None:
            asyncio.run_coroutine_threadsafe(self.shutdown(), self.loop)
        self.root.after(200, self.root.destroy)

    def poll_windows_volume(self):
        if (
            self.connected
            and self.client is not None
            and self.knob_mode == 2
            and time.monotonic() >= self.ignore_poll_until
        ):
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

    async def on_trigger(self, _sender, data):
        kind = data[0] if data else TRIGGER_ON
        if kind == TRIGGER_PLAY_PAUSE:
            send_play_pause()
            log("Media: play/pause")
            self.set_email_status("Play/Pause")
            return
        subject = FOCUS_OFF_SUBJECT if kind == TRIGGER_OFF else FOCUS_ON_SUBJECT
        self.set_email_status(f"Sending {subject}…")
        threading.Thread(
            target=self.send_focus_email_worker, args=(subject,), daemon=True
        ).start()

    def send_focus_email_worker(self, subject):
        try:
            send_focus_email(subject)
            log(f"Email: sent {subject}")
            self.set_email_status(f"Sent: {subject}")
        except Exception as exc:
            log(f"Email: failed: {exc}")
            self.set_email_status(f"Failed: {exc}")

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
        discovered = await BleakScanner.discover(timeout=8.0, return_adv=True)
        match = None
        lines = []
        for address, (device, adv) in discovered.items():
            name = device.name or adv.local_name or "?"
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            line = f"{name}  {address}  uuids={len(uuids)}"
            lines.append(line)
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
                log(f"BLE: knob not found ({seen} ads)")
                self.set_status(f"Not found ({seen} BLE ads) — retrying")
                await asyncio.sleep(2)
                continue

            log(f"BLE: found {DEVICE_NAME} at {device.address} ({seen} ads)")
            self.set_status(f"Connecting to {device.address}")
            try:
                async with BleakClient(device) as client:
                    self.client = client
                    self.connected = True
                    self.resolve_armed = False
                    self.set_status("Connected")
                    log(f"BLE: connected {device.address}")
                    await client.start_notify(STATUS_UUID, self.on_status)
                    try:
                        await client.start_notify(TRIGGER_UUID, self.on_trigger)
                    except Exception as exc:
                        log(f"Email: trigger subscribe failed: {exc}")
                        self.set_email_status(f"Trigger unavailable: {exc}")
                    percent = int(round(self.endpoint.GetMasterVolumeLevelScalar() * 100))
                    self.echo_percent = percent
                    if self.knob_mode == 2:
                        await self.write_percent(percent)
                    while client.is_connected:
                        await asyncio.sleep(0.2)
            except Exception as exc:
                log(f"BLE: error {exc}")
                traceback.print_exc()
                self.set_status(f"Error: {exc}")
            finally:
                self.connected = False
                self.client = None
                self.resolve_armed = False
                log("BLE: disconnected")
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
    log(f"Bridge: start Python {sys.version.split()[0]}")
    root = tk.Tk()
    app = KnobApp(root)
    app.start_ble_thread()
    root.mainloop()


if __name__ == "__main__":
    main()
