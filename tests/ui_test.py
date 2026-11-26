"""Exercise the actual WinUI window and Windows shell integration."""
import ctypes
import faulthandler
import sys
from ctypes import wintypes
import struct
from pathlib import Path
import re
import subprocess
import time
import traceback
import winreg
import win32api
import win32gui
from zipfile import ZipFile, ZIP_STORED

from pywinauto import Application, Desktop
from pywinauto.keyboard import send_keys

ROOT = Path.cwd()
EXE = ROOT / "artifacts/app/WinZipFS.exe"
FIX = ROOT / "artifacts/fixtures"
RESULTS = ROOT / "artifacts/test-results"
RESULTS.mkdir(parents=True, exist_ok=True)
desktop = Desktop(backend="uia")
user32 = ctypes.windll.user32
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowW.restype = ctypes.c_void_p
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL


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


app = None
window = None
main_hwnd = None


def ctl(identifier, bring_into_view=False):
    spec = window.child_window(auto_id=identifier, visible_only=False)
    if spec.exists(timeout=0.5, retry_interval=0.1):
        control = spec.wrapper_object()
        if not bring_into_view or control.is_visible():
            return control
    scroll = window.child_window(auto_id="DashboardScroll", visible_only=False).wrapper_object()
    for position in (0.0, 45.0, 75.0, 100.0):
        scroll.iface_scroll.SetScrollPercent(-1.0, position)
        time.sleep(0.35)
        if spec.exists(timeout=0.5, retry_interval=0.1):
            control = spec.wrapper_object()
            if not bring_into_view or control.is_visible():
                return control
    raise AssertionError(f"Control {identifier} is unavailable")


def click(identifier):
    print(f"Click {identifier}", flush=True)
    ctl(identifier).invoke()


def value(identifier, text):
    ctl(identifier).set_edit_text(str(text))
    assert ctl(identifier).iface_value.CurrentValue == str(text), identifier


COMBO_VALUES = {
    "Level": ("Store (0)", "Fastest (1)", "Fast (3)", "Normal (5)", "Maximum (7)", "Ultra (9)"),
    "Method": ("Automatic", "Store", "Deflate (ZIP)", "LZMA2 (7z)", "BZip2 (ZIP / 7z)"),
}


def combo(identifier, index):
    target = COMBO_VALUES[identifier][index]
    control = ctl(identifier, bring_into_view=True)
    control.select(target)
    wait_for(lambda: ctl(identifier).selected_text() == target, f"Selection failed for {identifier}")


def selected_index(identifier):
    return COMBO_VALUES[identifier].index(ctl(identifier).selected_text())


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
    window.set_focus()
    ctl("Mount", bring_into_view=True).click_input()
    complete_archive_dialog(path)


def complete_archive_dialog(path):
    print(f"Choose archive {path.name}", flush=True)
    handle = wait_for(lambda: user32.FindWindowW("#32770", "WinZipFS - Mount archive"),
                      "Archive picker did not open", timeout=30)
    win32 = Desktop(backend="win32")
    def descendants(parent):
        children = []
        win32gui.EnumChildWindows(parent, lambda child, _: children.append(child) or True, None)
        return children
    def filename_edit():
        for child in descendants(handle):
            if win32gui.GetDlgCtrlID(child) == 1148:
                for field in [child] + descendants(child):
                    if win32gui.GetClassName(field) == "Edit":
                        return field
        return None
    edit_handle = wait_for(filename_edit, "File name input not found", timeout=20)
    print("Set native filename", flush=True)
    field = win32.window(handle=edit_handle).wrapper_object()
    field.set_edit_text(str(path))
    button = wait_for(lambda: win32gui.GetDlgItem(handle, 1), "Open button missing")
    print("Confirm native picker", flush=True)
    win32.window(handle=button).wrapper_object().click_input()
    wait_for(lambda: not user32.IsWindowVisible(handle), "Archive picker did not close", timeout=30)

def tray_menu(keys):
    control = user32.FindWindowW("WinZipFS.Control", None)
    assert control, "Notification area callback window missing"
    user32.PostMessageW(control, 0x8001, 1, 0x0205)
    wait_for(lambda: user32.FindWindowW("#32768", None), "Notification menu did not open")
    send_keys(keys, pause=0.2)
    wait_for(lambda: not user32.FindWindowW("#32768", None), "Notification menu did not close")


def close_explorer():
    command = ("$shell=New-Object -ComObject Shell.Application; "
               "foreach($w in @($shell.Windows())) {try {$w.Quit()}catch{}}")
    subprocess.run(["powershell", "-NoProfile", "-Command", command], check=True)



class NotifyIconIdentifier(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("hWnd", wintypes.HWND),
                ("uID", wintypes.UINT), ("guidItem", ctypes.c_byte * 16)]


class ShellExecuteInfo(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
        ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR),
        ("lpParameters", wintypes.LPCWSTR), ("lpDirectory", wintypes.LPCWSTR),
        ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE), ("lpIDList", ctypes.c_void_p),
        ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY), ("dwHotKey", wintypes.DWORD),
        ("hIcon", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]


def verify_tray_icon():
    hwnd = wait_for(lambda: user32.FindWindowW("WinZipFS.Control", None), "Tray owner missing")
    info = NotifyIconIdentifier()
    info.cbSize = ctypes.sizeof(info)
    info.hWnd = hwnd
    info.uID = 1
    rect = wintypes.RECT()
    get_rect = ctypes.windll.shell32.Shell_NotifyIconGetRect
    get_rect.argtypes = [ctypes.POINTER(NotifyIconIdentifier), ctypes.POINTER(wintypes.RECT)]
    get_rect.restype = ctypes.c_long
    wait_for(lambda: get_rect(ctypes.byref(info), ctypes.byref(rect)) == 0,
             "Notification icon not registered with Explorer")


def open_associated(path):
    info = ShellExecuteInfo()
    info.cbSize = ctypes.sizeof(info)
    info.fMask = 0x00000001 | 0x00000100  # SEE_MASK_CLASSNAME | SEE_MASK_NOASYNC
    info.lpVerb = "open"
    info.lpFile = str(path)
    info.lpClass = "WinZipFS.Archive"
    info.nShow = 1
    execute = ctypes.windll.shell32.ShellExecuteExW
    execute.argtypes = [ctypes.POINTER(ShellExecuteInfo)]
    execute.restype = wintypes.BOOL
    assert execute(ctypes.byref(info)), "Registered open command failed"


def select_volume(name):
    item = wait_for(lambda: next((v for v in ctl("Volumes").descendants(control_type="ListItem")
                                  if name in v.window_text()), None), f"Missing volume {name}")
    item.select()
    wait_for(lambda: name in ctl("Details").window_text(), f"Selection failed for {name}")


def current_options():
    return (ctl("Batch").iface_toggle.CurrentToggleState,
            int(ctl("Delay").iface_value.CurrentValue),
            int(ctl("Idle").iface_value.CurrentValue),
            selected_index("Level"), selected_index("Method"),
            int(ctl("Threads").iface_value.CurrentValue),
            ctl("Solid").iface_toggle.CurrentToggleState)



def start_application():
    global app, window, main_hwnd
    app = Application(backend="uia").start(f'"{EXE}" --no-explorer')
    window = app.window(title="WinZipFS")
    window.wait("exists ready", timeout=90)
    main_hwnd = window.wrapper_object().handle
    work = win32api.GetMonitorInfo(win32api.MonitorFromWindow(main_hwnd, 2))["Work"]
    rect = win32gui.GetWindowRect(main_hwnd)
    assert rect[0] >= work[0] and rect[1] >= work[1] and rect[2] <= work[2] and rect[3] <= work[3], (rect, work)
    window = app.window(handle=main_hwnd, visible_only=False)
    verify_tray_icon()


def launch_archive(path):
    subprocess.run([str(EXE), str(path)], timeout=30, check=True)
    status_contains("Mounted")
    select_volume(path.name)
    drive = drive_from_details()
    return Path(drive + "\\")


def eject_selected():
    root = Path(drive_from_details() + "\\")
    wait_closed()
    click("Eject")
    status_contains("Ejected")
    wait_for(lambda: not root.exists(), "Drive remains mounted")


def exit_application():
    print("Click Exit", flush=True)
    window.set_focus()
    ctl("Exit", bring_into_view=True).click_input()
    app.wait_for_process_exit(timeout=60)


def test_startup():
    verify_tray_icon()
    for name in ("DashboardScroll", "Mount", "Hide", "Exit", "Status", "Summary", "Volumes", "Details",
                 "Open", "Flush", "Eject", "Batch", "Delay", "Idle", "Level", "Method", "Threads",
                 "Solid", "Apply", "LoadDefaults", "SaveDefaults", "Register", "Unregister",
                 "DefaultApps", "Recovery"):
        print(f"Locate {name}", flush=True)
        ctl(name)
    print("Test progress during a real archive mount", flush=True)
    progress_archive = FIX / "progress.zip"
    with ZipFile(progress_archive, "w", compression=ZIP_STORED) as archive:
        for i in range(10000):
            archive.writestr(f"entries/{i:05d}.txt", f"entry {i}")
    launch = subprocess.Popen([str(EXE), str(progress_archive)])
    progress_spec = window.child_window(auto_id="Progress", visible_only=False)
    wait_for(lambda: "Working" in status() and progress_spec.exists(timeout=0.1, retry_interval=0.05),
             "Active progress ring was not exposed during mount", timeout=30)
    assert launch.wait(timeout=30) == 0
    status_contains("Mounted")
    progress_drive = drive_from_details()
    assert (Path(progress_drive + "\\") / "entries" / "09999.txt").read_text() == "entry 9999"
    wait_closed()
    click("Eject")
    status_contains("Ejected")
    exit_application()


def test_picker():
    choose_archive(FIX / "ui.zip")
    status_contains("Mounted")
    drive = drive_from_details()
    root = Path(drive + "\\")
    assert (root / "sample.txt").read_text() == "original"
    eject_selected()
    exit_application()


def test_shell():
    click("Register")
    status_contains("registered")
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\WinZipFS\Capabilities\FileAssociations") as key:
        for ext in (".zip", ".zipx", ".7z", ".rar", ".tar", ".gz", ".gzip", ".tgz", ".bz2", ".bzip2",
                    ".tbz", ".tbz2", ".xz", ".txz", ".zst", ".tzst", ".cab", ".arj", ".lha", ".lzh",
                    ".z", ".cpio", ".deb", ".rpm", ".iso", ".wim", ".swm", ".esd", ".dmg", ".xar",
                    ".ar", ".001", ".chm", ".lzma", ".udf", ".squashfs"):
            assert winreg.QueryValueEx(key, ext)[0] == "WinZipFS.Archive", ext
    click("DefaultApps")
    wait_for(lambda: "Windows default apps" in status(), "Default apps command did not report a result")
    open_associated(FIX / "readonly.cab")
    status_contains("Mounted")
    select_volume("readonly.cab")
    readonly_drive = drive_from_details()
    assert "Read-only" in ctl("Details").window_text()
    assert not ctl("Flush").is_enabled()
    wait_closed()
    click("Eject")
    status_contains("Ejected")
    wait_for(lambda: not Path(readonly_drive + "\\").exists(), "Read-only drive still mounted")
    click("Unregister")
    status_contains("removed")
    try:
        winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Classes\WinZipFS.Archive")
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("ProgID was not removed")
    exit_application()


def test_settings():
    root = launch_archive(FIX / "ui.zip")
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
    assert current_options() == (1, 8, 2, 4, 2, 2, 0)
    window.capture_as_image().save(RESULTS / "dashboard-settings.png")
    value("Delay", 0)
    click("Apply")
    status_contains("Error:")
    value("Delay", 8)
    subprocess.run([str(EXE), str(FIX / "readonly.cab")], timeout=30, check=True)
    status_contains("Mounted")
    select_volume("readonly.cab")
    assert "Read-only" in ctl("Details").window_text()
    assert not ctl("Flush").is_enabled()
    eject_selected()
    select_volume("ui.zip")
    assert current_options() == (1, 8, 2, 4, 2, 2, 0)
    (root / "ui.txt").write_text("WinUI writeback", encoding="utf-8")
    wait_closed()
    click("Flush")
    status_contains("Saved")
    with ZipFile(FIX / "ui.zip", "r") as archive:
        assert archive.read("ui.txt") == b"WinUI writeback"
    click("Open")
    status_contains("Opened ")
    time.sleep(2)
    close_explorer()
    click("Recovery")
    status_contains("Opened recovery cache")
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
        assert struct.unpack("<7i", winreg.QueryValueEx(key, "Options")[0]) == (0, 3, 0, 1, 0, 1, 1)
    click("Hide")
    wait_for(lambda: not user32.IsWindowVisible(main_hwnd), "Hide failed")
    subprocess.run([str(EXE)], timeout=30, check=True)
    wait_for(lambda: user32.IsWindowVisible(main_hwnd), "Single instance show failed")
    wait_closed()
    click("Eject")
    status_contains("Ejected")
    wait_for(lambda: not root.exists(), "Ejected drive still exists")
    exit_application()
    start_application()
    assert current_options() == (0, 3, 0, 1, 0, 1, 1)
    exit_application()


def test_tray_picker():
    tray_menu("{HOME}{DOWN}{ENTER}")
    complete_archive_dialog(FIX / "tray.7z")
    status_contains("Mounted")
    assert (Path(drive_from_details() + "\\") / "sample.txt").read_text() == "original"
    eject_selected()
    exit_application()


def test_tray():
    launch_archive(FIX / "tray.7z")
    tray_drive = drive_from_details()
    tray_root = Path(tray_drive + "\\")
    user32.PostMessageW(main_hwnd, 0x0010, 0, 0)
    wait_for(lambda: not user32.IsWindowVisible(main_hwnd), "Window close did not hide")
    tray_menu("{HOME}{ENTER}")
    wait_for(lambda: user32.IsWindowVisible(main_hwnd), "Tray Open action failed")
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{ENTER}")
    status_contains("Opened ")
    time.sleep(1)
    close_explorer()
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{ENTER}")
    assert current_options() == (0, 5, 0, 3, 0, 0, 1)
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{ENTER}")
    status_contains("Batch mode updated")
    assert ctl("Batch").iface_toggle.CurrentToggleState == 1
    (tray_root / "tray.txt").write_text("tray writeback", encoding="utf-8")
    wait_closed()
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{DOWN}{ENTER}")
    status_contains("Saved")
    tray_menu("{HOME}{DOWN}{DOWN}{RIGHT}{DOWN}{DOWN}{DOWN}{DOWN}{ENTER}")
    status_contains("Ejected")
    wait_for(lambda: not tray_root.exists(), "Tray eject failed")
    seven_zip = Path(r"C:\Program Files\7-Zip\7z.exe")
    result = subprocess.run([str(seven_zip), "x", "-so", str(FIX / "tray.7z"), "tray.txt"],
                            check=True, capture_output=True)
    assert result.stdout == b"tray writeback"
    tray_menu("{END}{ENTER}")
    app.wait_for_process_exit(timeout=60)


SUITES = {
    "startup": test_startup,
    "picker": test_picker,
    "shell": test_shell,
    "settings": test_settings,
    "tray-picker": test_tray_picker,
    "tray": test_tray,
}


if __name__ == "__main__":
    suite = sys.argv[1] if len(sys.argv) == 2 else None
    if suite not in SUITES:
        raise SystemExit("Usage: python tests/ui_test.py " + "|".join(SUITES))
    faulthandler.enable()
    faulthandler.dump_traceback_later(90, repeat=True)
    try:
        start_application()
        SUITES[suite]()
        (RESULTS / (suite + ".txt")).write_text(suite + " passed\n", encoding="utf-8")
    except Exception:
        (RESULTS / (suite + ".txt")).write_text(traceback.format_exc(), encoding="utf-8")
        try:
            for top in Desktop(backend="win32").windows():
                print("Window:", top.handle, top.process_id(), top.class_name(), repr(top.window_text()), flush=True)
        except Exception:
            pass
        try:
            if window is not None:
                window.capture_as_image().save(RESULTS / (suite + "-failure.png"))
        except Exception:
            pass
        try:
            if window is not None:
                import contextlib
                with (RESULTS / (suite + "-controls.txt")).open("w", encoding="utf-8") as out:
                    with contextlib.redirect_stdout(out):
                        window.print_control_identifiers()
        except Exception:
            pass
        raise
    finally:
        faulthandler.cancel_dump_traceback_later()
        if app is not None and app.is_process_running():
            app.kill()
