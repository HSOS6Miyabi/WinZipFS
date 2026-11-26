"""Exercise the actual WinUI window and Windows shell integration."""
import ctypes
from pathlib import Path
import re
import subprocess
import time
import traceback
import winreg

from pywinauto import Application, Desktop
from pywinauto.keyboard import send_keys

ROOT = Path.cwd()
EXE = ROOT / "artifacts/app/WinZipFS.exe"
FIX = ROOT / "artifacts/fixtures"
RESULTS = ROOT / "artifacts/test-results"
RESULTS.mkdir(parents=True, exist_ok=True)
desktop = Desktop(backend="uia")
user32 = ctypes.windll.user32
user32.FindWindowW.restype = ctypes.c_void_p
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]


def wait_for(predicate, message, timeout=60):
    until = time.monotonic() + timeout
    last = None
    while time.monotonic() < until:
        try:
            if result := predicate():
                return result
        except Exception as exc:
            last = exc
        time.sleep(0.25)
    raise AssertionError(f"{message}; {last}")


app = Application(backend="uia").start(f'"{EXE}" --no-explorer')
window = app.window(title="WinZipFS")
window.wait("exists ready", timeout=90)


def ctl(identifier):
    return window.child_window(auto_id=identifier).wrapper_object()


def click(identifier):
    ctl(identifier).invoke()


def value(identifier, text):
    ctl(identifier).set_edit_text(str(text))


def combo(identifier, index):
    ctl(identifier).select(index)


def toggle(identifier):
    ctl(identifier).iface_toggle.Toggle()


def status():
    return ctl("Status").window_text()


def status_contains(s):
    return wait_for(lambda: s in status(), f"Status did not contain {s!r}")


def wait_closed():
    wait_for(lambda: "Open handles: 0" in ctl("Details").window_text(), "Open file handles remain")


def drive_from_details():
    return wait_for(lambda: re.search(r"\b([D-Z]:)\s+·", ctl("Details").window_text()),
                    "Mounted drive missing").group(1)


def choose_archive(path):
    click("Mount")
    dialog = desktop.window(title="Open", control_type="Window")
    dialog.wait("exists ready", timeout=30)
    dialog.child_window(auto_id="1148", control_type="Edit").set_edit_text(str(path))
    dialog.child_window(title="Open", control_type="Button").invoke()


def tray_menu(keys):
    control = user32.FindWindowW("WinZipFS.Control", None)
    assert control, "Notification area callback window missing"
    user32.PostMessageW(control, 0x8001, 1, 0x0205)
    time.sleep(1)
    send_keys(keys, pause=0.2)


def close_explorer():
    command = ("$shell=New-Object -ComObject Shell.Application; "
               "foreach($w in @($shell.Windows())) {try {$w.Quit()}catch{}}")
    subprocess.run(["powershell", "-NoProfile", "-Command", command], check=True)


try:
    for name in ("Mount", "Hide", "Exit", "Progress", "Status", "Summary", "Volumes", "Details",
                 "Open", "Flush", "Eject", "Batch", "Delay", "Idle", "Level", "Method", "Threads",
                 "Solid", "Apply", "LoadDefaults", "SaveDefaults", "Register", "Unregister",
                 "DefaultApps", "Recovery"):
        ctl(name)
    click("Register")
    status_contains("registered")
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\WinZipFS\Capabilities\FileAssociations") as key:
        assert winreg.QueryValueEx(key, ".zip")[0] == "WinZipFS.Archive"
        assert winreg.QueryValueEx(key, ".rar")[0] == "WinZipFS.Archive"
    click("DefaultApps")
    time.sleep(2)
    click("Unregister")
    status_contains("removed")
    try:
        winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\WinZipFS.Archive")
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("ProgID was not removed")
    choose_archive(FIX / "ui.zip")
    status_contains("Mounted")
    drive = drive_from_details()
    root = Path(drive + "\\")
    assert (root / "sample.txt").read_text() == "original"
    toggle("Batch")
    value("Delay", 8)
    value("Idle", 2)
    combo("Level", 4)
    combo("Method", 2)
    value("Threads", 2)
    toggle("Solid")
    click("Apply")
    status_contains("Settings updated")
    (root / "ui.txt").write_text("WinUI writeback", encoding="utf-8")
    wait_closed()
    click("Flush")
    status_contains("Saved")
    click("Open")
    time.sleep(2)
    close_explorer()
    click("Recovery")
    time.sleep(2)
    close_explorer()
    click("LoadDefaults")
    status_contains("Default settings loaded")
    value("Delay", 3)
    value("Idle", 0)
    value("Threads", 1)
    combo("Method", 0)
    combo("Level", 1)
    click("SaveDefaults")
    status_contains("Defaults saved")
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\WinZipFS") as key:
        assert len(winreg.QueryValueEx(key, "Options")[0]) == 28
    click("Hide")
    wait_for(lambda: not window.is_visible(), "Hide failed")
    subprocess.run([str(EXE)], timeout=30, check=True)
    wait_for(window.is_visible, "Single instance show failed")
    wait_closed()
    click("Eject")
    status_contains("Ejected")
    wait_for(lambda: not root.exists(), "Ejected drive still exists")
    subprocess.run([str(EXE), str(FIX / "tray.7z")], timeout=30, check=True)
    status_contains("Mounted")
    tray_drive = drive_from_details()
    tray_root = Path(tray_drive + "\\")
    click("Hide")
    tray_menu("{HOME}{ENTER}")
    wait_for(window.is_visible, "Tray Open action failed")
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{ENTER}")
    status_contains("Batch mode updated")
    (tray_root / "tray.txt").write_text("tray writeback", encoding="utf-8")
    wait_closed()
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{DOWN}{ENTER}")
    status_contains("Saved")
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{DOWN}{DOWN}{ENTER}")
    status_contains("Ejected")
    wait_for(lambda: not tray_root.exists(), "Tray eject failed")
    click("Exit")
    app.wait_for_process_exit(timeout=60)
    (RESULTS / "ui.txt").write_text("WinUI and tray automation passed\n")
except Exception:
    (RESULTS / "ui.txt").write_text(traceback.format_exc())
    try:
        window.capture_as_image().save(RESULTS / "ui-failure.png")
    except Exception:
        pass
    raise
finally:
    if app.is_process_running():
        app.kill()
