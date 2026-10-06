"""Platform specifics, kept small and behind plain functions.

Everything else in ppharness is portable; anything that differs between Windows,
Linux and macOS lives here.
"""

from __future__ import annotations

import contextlib
import os
import shutil
import signal
import subprocess
import sys
import threading
from pathlib import Path
from typing import Any, Iterator

IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")


# --------------------------------------------------------------------------- binaries


def exe_name(stem: str) -> str:
    """'DolphinNoGUI' -> 'DolphinNoGUI.exe' on Windows."""
    return stem + ".exe" if IS_WINDOWS else stem


def default_binaries_dir(dolphin_root: Path) -> Path:
    if IS_WINDOWS:
        return dolphin_root / "build" / "release" / "x64" / "Binaries"
    if IS_MACOS:
        return dolphin_root / "build" / "Binaries"
    return dolphin_root / "build" / "Binaries"


def default_screenshot_setup() -> tuple[str, str]:
    """(platform, video backend) to use when a real frame buffer is needed."""
    if IS_WINDOWS:
        return "headless", "D3D11"   # verified: renders offscreen, no window appears
    if IS_MACOS:
        return "headless", "Vulkan"  # untested
    return "headless", "Vulkan"      # untested


# --------------------------------------------------------------------------- file copy


def fast_copy_file(src: str | os.PathLike[str], dst: str | os.PathLike[str]) -> None:
    """Copy one file as fast as the OS allows (clone/reflink where supported)."""
    src_s, dst_s = os.fspath(src), os.fspath(dst)
    if IS_WINDOWS:
        if _win_copyfile(src_s, dst_s):
            return
    elif IS_LINUX:
        if _linux_reflink(src_s, dst_s):
            shutil.copystat(src_s, dst_s)
            return
    elif IS_MACOS:
        if _macos_clonefile(src_s, dst_s):
            return
    # Portable fallback (uses sendfile / fcopyfile where Python supports it).
    shutil.copy2(src_s, dst_s)


def _win_copyfile(src: str, dst: str) -> bool:
    try:
        import ctypes
        from ctypes import wintypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CopyFileW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.BOOL]
        k32.CopyFileW.restype = wintypes.BOOL
        # CopyFileW uses the kernel's cached copy path (and block cloning on ReFS/Dev Drive).
        return bool(k32.CopyFileW(src, dst, False))
    except (OSError, AttributeError):
        return False


def _linux_reflink(src: str, dst: str) -> bool:
    try:
        import fcntl
        FICLONE = 0x40049409
        with open(src, "rb") as fs, open(dst, "wb") as fd:
            fcntl.ioctl(fd.fileno(), FICLONE, fs.fileno())
        return True
    except (OSError, ImportError):
        with contextlib.suppress(OSError):
            os.unlink(dst)
        return False


def _macos_clonefile(src: str, dst: str) -> bool:
    try:
        import ctypes
        libc = ctypes.CDLL("/usr/lib/libSystem.dylib", use_errno=True)
        return libc.clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0
    except (OSError, AttributeError):
        return False


def link_dir(target: Path, link: Path) -> str:
    """Make ``link`` point at directory ``target`` without copying.

    Returns the kind of link made ("symlink" or "junction"). Raises OSError if the
    platform cannot link (caller then copies instead).
    """
    try:
        os.symlink(target, link, target_is_directory=True)
        return "symlink"
    except OSError:
        if not IS_WINDOWS:
            raise
    # Windows without symlink privilege: a directory junction needs no privilege.
    import _winapi  # type: ignore[import-not-found]
    _winapi.CreateJunction(str(target), str(link))
    return "junction"


def unlink_dir(link: Path) -> None:
    """Remove a directory symlink/junction made by link_dir (never its target)."""
    if IS_WINDOWS:
        os.rmdir(link)  # removes the reparse point only
    else:
        os.unlink(link)


# --------------------------------------------------------------------------- processes


_job_lock = threading.Lock()
_job_handle: Any = None


def popen_kwargs() -> dict[str, Any]:
    """Extra subprocess.Popen kwargs: own process group, no console window."""
    if IS_WINDOWS:
        flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        return {"creationflags": flags}
    kw: dict[str, Any] = {"start_new_session": True}
    if IS_LINUX and threading.current_thread() is threading.main_thread():
        # PDEATHSIG fires when the *thread* that forked exits, so only use it from main.
        kw["preexec_fn"] = _linux_set_pdeathsig
    return kw


def _linux_set_pdeathsig() -> None:
    try:
        import ctypes
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        PR_SET_PDEATHSIG = 1
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL)
    except OSError:
        pass


def bind_to_parent(proc: subprocess.Popen) -> None:
    """Make sure ``proc`` dies if this Python process dies (best effort).

    Windows: a Job object with KILL_ON_JOB_CLOSE. Linux: PDEATHSIG (set in
    popen_kwargs). macOS: nothing, we rely on atexit cleanup.
    """
    if not IS_WINDOWS:
        return
    global _job_handle
    try:
        import ctypes
        from ctypes import wintypes

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        with _job_lock:
            if _job_handle is None:
                k32.CreateJobObjectW.restype = wintypes.HANDLE
                job = k32.CreateJobObjectW(None, None)
                if not job:
                    return

                class IO_COUNTERS(ctypes.Structure):
                    _fields_ = [(n, ctypes.c_ulonglong) for n in (
                        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

                class BASIC(ctypes.Structure):
                    _fields_ = [
                        ("PerProcessUserTimeLimit", ctypes.c_int64),
                        ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD),
                        ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t),
                        ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t),
                        ("PriorityClass", wintypes.DWORD),
                        ("SchedulingClass", wintypes.DWORD),
                    ]

                class EXTENDED(ctypes.Structure):
                    _fields_ = [
                        ("BasicLimitInformation", BASIC),
                        ("IoInfo", IO_COUNTERS),
                        ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t),
                    ]

                info = EXTENDED()
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
                JobObjectExtendedLimitInformation = 9
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                k32.SetInformationJobObject.argtypes = [
                    wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
                if not k32.SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                                   ctypes.byref(info), ctypes.sizeof(info)):
                    k32.CloseHandle(job)
                    return
                _job_handle = job
            k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
            k32.AssignProcessToJobObject(_job_handle, wintypes.HANDLE(int(proc._handle)))  # type: ignore[attr-defined]
    except (OSError, AttributeError):
        pass


def request_graceful_exit(proc: subprocess.Popen) -> bool:
    """Ask the process to exit cleanly via a signal. Returns False if unsupported.

    Dolphin handles SIGTERM/SIGINT by stopping the core and flushing logs. On Windows
    there is no reliable way to deliver a console signal to a process in its own
    group, so we only rely on the harness ``quit`` command there.
    """
    if IS_WINDOWS:
        return False
    try:
        proc.send_signal(signal.SIGTERM)
        return True
    except OSError:
        return False


def kill_process_tree(proc: subprocess.Popen) -> None:
    """Forcefully kill ``proc`` and anything in its process group."""
    if proc.poll() is not None:
        return
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        if proc.poll() is None:
            with contextlib.suppress(OSError):
                proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (OSError, AttributeError):
        with contextlib.suppress(OSError):
            proc.kill()


def pid_alive(pid: int) -> bool:
    if IS_WINDOWS:
        import ctypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            return False
        try:
            code = ctypes.c_ulong()
            ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
            return bool(ok) and code.value == STILL_ACTIVE
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


# --------------------------------------------------------------------------- timers


@contextlib.contextmanager
def high_resolution_timer() -> Iterator[bool]:
    """Raise the OS timer resolution to ~1 ms while inside the block.

    On Windows the default tick is ~15.6 ms, which makes Condition.wait()/select()
    timeouts overshoot by up to that much. timeBeginPeriod(1) fixes it (per process on
    Windows 10 2004+). Yields True if the resolution was raised. Other OSes already
    have fine-grained timers.
    """
    if not IS_WINDOWS:
        yield True
        return
    winmm = None
    try:
        import ctypes
        winmm = ctypes.WinDLL("winmm")
        ok = winmm.timeBeginPeriod(1) == 0
    except OSError:
        ok = False
    try:
        yield ok
    finally:
        if ok and winmm is not None:
            winmm.timeEndPeriod(1)


def boost_current_thread() -> bool:
    """Raise the calling thread's scheduling priority (best effort, no privileges needed).

    Used for netsim's timing threads so a busy machine (e.g. a Dolphin build) delays
    packets less. Only does something on Windows; elsewhere raising priority needs
    privileges, so this is a no-op.
    """
    if not IS_WINDOWS:
        return False
    try:
        import ctypes
        k32 = ctypes.WinDLL("kernel32")
        k32.GetCurrentThread.restype = ctypes.c_void_p
        k32.SetThreadPriority.argtypes = [ctypes.c_void_p, ctypes.c_int]
        THREAD_PRIORITY_HIGHEST = 2
        return bool(k32.SetThreadPriority(k32.GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
    except (OSError, AttributeError):
        return False


def coarse_wait_slack() -> float:
    """How early (seconds) to stop a Condition.wait before a deadline and fine-sleep."""
    return 0.002
