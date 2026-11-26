"""Exercise the actual WinUI window and Windows shell integration."""
import ctypes
import os
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

from pywinauto import Application, Desktop, mouse
from pywinauto.keyboard import send_keys

ROOT = Path.cwd()
EXE = Path(os.environ.get("WZ_EXE", ROOT / "artifacts/app/WinZipFS.exe")).resolve()
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
    ctl(identifier, bring_into_view=True).set_edit_text(str(text))
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
    ctl(identifier, bring_into_view=True).iface_toggle.Toggle()


def status():
    return ctl("Status").window_text()


def status_contains(s):
    return wait_for(lambda: s in status(), f"Status did not contain {s!r}")


def wait_closed():
    wait_for(lambda: "Open handles: 0" in ctl("Details").window_text(), "Open file handles remain")


def drive_from_details():
    return wait_for(lambda: re.search(r"\b([D-Z]:)\s+·", ctl("Details").window_text()),
                    "Mounted drive missing").group(1)


def choose_archive(path, identifier="Mount"):
    window.set_focus()
    ctl(identifier, bring_into_view=True).click_input()
    complete_archive_dialog(path)


def complete_archive_dialog(path, title="WinZipFS - Mount archive", confirmation="1"):
    print(f"Choose archive {path.name}", flush=True)
    win32 = Desktop(backend="win32")
    def find_dialog():
        owned = window.child_window(title=title, control_type="Window", visible_only=False)
        if owned.exists(timeout=0.2, retry_interval=0.1):
            return owned.wrapper_object().handle
        return next((w.handle for w in win32.windows(process=app.process, class_name="#32770")
                     if w.window_text() == title), None)
    handle = wait_for(find_dialog, "Archive picker did not open", timeout=30)
    dialog = desktop.window(handle=handle, visible_only=False)
    field_id = "1001" if title == "WinZipFS - Save archive copy" else "1148"
    field = dialog.child_window(auto_id=field_id, control_type="Edit", visible_only=False)
    field.wait("exists enabled", timeout=20)
    if title == "WinZipFS - Save archive copy":
        field.click_input()
        send_keys("^a")
        send_keys(str(path), with_spaces=True)
        send_keys("{TAB}")
    else:
        field.set_edit_text(str(path))
    dialog.child_window(auto_id=confirmation, control_type="Button", visible_only=False).invoke()
    wait_for(lambda: not user32.IsWindowVisible(handle), "Archive picker did not close", timeout=30)


def popup_windows():
    return Desktop(backend="win32").windows(process=app.process, class_name="#32768", visible_only=True)


def popup_item(label):
    # Read the native popup's actual labels and item rectangles. The popup is
    # owned by WinZipFS, so unrelated system menus are never selected.
    get_menu = user32.SendMessageTimeoutW
    get_menu.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t,
                         wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    get_menu.restype = ctypes.c_ssize_t
    user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
    user32.GetMenuItemCount.restype = ctypes.c_int
    user32.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]
    user32.GetMenuStringW.restype = ctypes.c_int
    user32.GetMenuItemRect.argtypes = [wintypes.HWND, wintypes.HMENU, wintypes.UINT, ctypes.POINTER(wintypes.RECT)]
    user32.GetMenuItemRect.restype = wintypes.BOOL
    for popup in popup_windows():
        menu = ctypes.c_size_t()
        if not get_menu(popup.handle, 0x01E1, 0, 0, 2, 1000, ctypes.byref(menu)) or not menu.value:
            continue
        for index in range(user32.GetMenuItemCount(menu.value)):
            name = ctypes.create_unicode_buffer(512)
            user32.GetMenuStringW(menu.value, index, name, len(name), 0x400)  # MF_BYPOSITION
            if name.value != label:
                continue
            rect = wintypes.RECT()
            if user32.GetMenuItemRect(None, menu.value, index, ctypes.byref(rect)) and rect.right > rect.left:
                return ((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
    return None


def tray_menu(*labels):
    print("Tray menu:", " > ".join(labels), flush=True)
    control = user32.FindWindowW("WinZipFS.Control", None)
    assert control, "Notification area callback window missing"
    # Use the registered callback to open the real native popup, then perform
    # mouse selection on its displayed items rather than relying on keyboard focus.
    assert user32.PostMessageW(control, 0x8001, 1, 0x0205)
    wait_for(popup_windows, "Notification menu did not open", timeout=15)
    for label in labels[:-1]:
        point = wait_for(lambda: popup_item(label), f"Menu item unavailable: {label}", timeout=15)
        mouse.move(coords=point)
        time.sleep(0.6)
    point = wait_for(lambda: popup_item(labels[-1]), f"Menu item unavailable: {labels[-1]}", timeout=15)
    mouse.click(coords=point)
    wait_for(lambda: not popup_windows(), "Notification menu did not close", timeout=15)

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
    # The application enables Hide only after Shell_NotifyIcon(NIM_ADD)
    # succeeds. Explorer may retain registered icons in its overflow area;
    # a screen rectangle is not required to be available for those icons.
    wait_for(lambda: ctl("Hide").is_enabled(), "Explorer did not accept notification icon registration")
    result = get_rect(ctypes.byref(info), ctypes.byref(rect))
    print("Tray icon registered; Shell_NotifyIconGetRect:",
          f"0x{result & 0xffffffff:08x}", (rect.left, rect.top, rect.right, rect.bottom), flush=True)


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
    status_contains("No abandoned archive sessions")
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
    tray_menu("Mount archive…")
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
    tray_menu("Open WinZipFS")
    wait_for(lambda: user32.IsWindowVisible(main_hwnd), "Tray Open action failed")
    tray_menu(tray_drive + "  tray.7z", "Open drive")
    status_contains("Opened ")
    time.sleep(1)
    close_explorer()
    tray_menu(tray_drive + "  tray.7z", "Compression and cache settings")
    assert current_options() == (0, 5, 0, 3, 0, 0, 1)
    tray_menu(tray_drive + "  tray.7z", "Batch write mode")
    status_contains("Batch mode updated")
    assert ctl("Batch").iface_toggle.CurrentToggleState == 1
    (tray_root / "tray.txt").write_text("tray writeback", encoding="utf-8")
    wait_closed()
    tray_menu(tray_drive + "  tray.7z", "Save changes")
    status_contains("Saved")
    tray_menu(tray_drive + "  tray.7z", "Eject")
    status_contains("Ejected")
    wait_for(lambda: not tray_root.exists(), "Tray eject failed")
    seven_zip = Path(r"C:\Program Files\7-Zip\7z.exe")
    result = subprocess.run([str(seven_zip), "x", "-so", str(FIX / "tray.7z"), "tray.txt"],
                            check=True, capture_output=True)
    assert result.stdout == b"tray writeback"
    tray_menu("Exit")
    app.wait_for_process_exit(timeout=60)


def test_safety():
    choose_archive(FIX / "ui.zip", "MountReadOnly")
    status_contains("Mounted")
    root = Path(drive_from_details() + "\\")
    assert "Read-only" in ctl("Details").window_text()
    assert not ctl("Flush").is_enabled()
    assert not ctl("Discard").is_enabled()
    eject_selected()
    root = launch_archive(FIX / "ui.zip")
    toggle("Batch")
    click("Apply")
    status_contains("Settings updated")
    (root / "unsaved.txt").write_text("safe copy", encoding="utf-8")
    wait_closed()
    wait_for(lambda: ctl("Discard").is_enabled(), "Pending edits not reflected")
    target = FIX / "saved-copy.zip"
    window.set_focus()
    ctl("SaveCopy", bring_into_view=True).click_input()
    complete_archive_dialog(target, title="WinZipFS - Save archive copy")
    status_contains("Saved copy: " + str(target))
    wait_for(target.exists, "Saved copy missing")
    with ZipFile(target) as archive:
        assert archive.read("unsaved.txt") == b"safe copy"
    with ZipFile(FIX / "ui.zip") as archive:
        assert "unsaved.txt" not in archive.namelist()
    def discard(confirm):
        window.set_focus()
        ctl("Discard", bring_into_view=True).click_input()
        dialog = Desktop(backend="win32").window(title="WinZipFS - Discard changes", class_name="#32770")
        dialog.wait("exists ready", timeout=20)
        dialog.child_window(control_id=6 if confirm else 7, class_name="Button").click_input()
    discard(False)
    assert (root / "unsaved.txt").read_text() == "safe copy"
    wait_closed()
    discard(True)
    status_contains("Discarded changes")
    wait_for(lambda: not root.exists(), "Discarded drive remains mounted")
    with ZipFile(FIX / "ui.zip") as archive:
        assert "unsaved.txt" not in archive.namelist()
    exit_application()


def modal_button(label):
    button = wait_for(lambda: next((item for item in window.descendants(control_type="Button")
                                   if item.window_text() == label and item.is_visible()), None),
                      f"Dialog button missing: {label}")
    button.click_input()


def test_password():
    subprocess.run([str(EXE), str(FIX / "encrypted.7z")], timeout=30, check=True)
    password = ctl("ArchivePassword", bring_into_view=True)
    password.click_input()
    send_keys("wrong-password", with_spaces=True)
    modal_button("Mount read-only")
    wait_for(lambda: window.child_window(auto_id="ArchivePassword", visible_only=False).exists(timeout=0.5),
             "Password was not requested after failure")
    password = ctl("ArchivePassword", bring_into_view=True)
    password.click_input()
    send_keys("TestFixturePassword", with_spaces=True)
    modal_button("Mount read-only")
    status_contains("Mounted")
    root = Path(drive_from_details() + "\\")
    assert (root / "sample.txt").read_text() == "original"
    assert "Read-only" in ctl("Details").window_text()
    assert not ctl("Flush").is_enabled()
    assert not ctl("SaveCopy").is_enabled()
    eject_selected()
    exit_application()


def test_recovery():
    root = launch_archive(FIX / "ui.zip")
    toggle("Batch")
    click("Apply")
    status_contains("Settings updated")
    (root / "recovery-ui.txt").write_text("recovered UI data", encoding="utf-8")
    wait_closed()
    app.kill(soft=False)
    app.wait_for_process_exit(timeout=30)
    start_application()
    status_contains("Recovered archive sessions")
    click("Recovery")
    ctl("RecoveryList")
    modal_button("Save recovered copy…")
    destination = FIX / "recovery-ui.zip"
    complete_archive_dialog(destination, title="WinZipFS - Save archive copy")
    status_contains("Recovered copy: " + str(destination))
    wait_for(destination.exists, "Recovered copy missing")
    with ZipFile(destination) as archive:
        assert archive.read("recovery-ui.txt") == b"recovered UI data"
    click("Recovery")
    ctl("RecoveryList")
    modal_button("Delete recovery cache…")
    dialog = Desktop(backend="win32").window(title="WinZipFS - Delete recovery cache", class_name="#32770")
    dialog.wait("exists ready", timeout=20)
    dialog.child_window(control_id=6, class_name="Button").click_input()
    status_contains("Recovery cache removed")
    click("Recovery")
    status_contains("No abandoned archive sessions")
    exit_application()


SUITES = {
    "startup": test_startup,
    "safety": test_safety,
    "password": test_password,
    "recovery": test_recovery,
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
