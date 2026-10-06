"""DolphinInstance lifecycle, using fake_dolphin (a Python process) instead of Dolphin."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

from ppharness import _platform
from ppharness.client import HarnessClient
from ppharness.inifile import IniFile
from ppharness.instance import (
    DolphinInstance,
    InstanceConfig,
    InstanceError,
    clean_instances,
    copy_template,
    remove_tree,
)
from ppharness.logtail import LogTail, LogTimeoutError


# --------------------------------------------------------------------------- INI editing


def test_inifile_edits_in_place(tmp_path):
    p = tmp_path / "Dolphin.ini"
    p.write_text("; comment\n[Core]\nCPUThread = True\nsidevice0 = 12\n\n[DSP]\nVolume = 50\n")
    ini = IniFile.load(p)
    ini.set("Core", "SIDevice0", 6)          # case-insensitive match, replaced in place
    ini.set("Core", "GFXBackend", "Null")    # appended to its section
    ini.set("DSP", "Muted", True)
    ini.set("NetPlay", "TraversalChoice", "direct")  # new section
    ini.save(p)
    text = p.read_text()
    assert text.startswith("; comment\n[Core]\nCPUThread = True\nSIDevice0 = 6\nGFXBackend = Null\n")
    assert "[DSP]\nVolume = 50\nMuted = True\n" in text
    assert text.rstrip().endswith("[NetPlay]\nTraversalChoice = direct")
    ini2 = IniFile.load(p)
    assert ini2.get("core", "sidevice0") == "6"
    assert ini2.get("Core", "Missing") is None
    ini2.replace_section("Core", {"A": 1})
    assert ini2.get("Core", "CPUThread") is None and ini2.get("Core", "A") == "1"
    assert ini2.remove("DSP", "Volume") and ini2.get("DSP", "Volume") is None


# --------------------------------------------------------------------------- template copy


def test_copy_template_links_load_and_empties_logs(template, tmp_path):
    dest = tmp_path / "copy"
    linked = copy_template(template, dest)
    assert linked == ["Load"]
    assert (dest / "Load" / "Textures" / "pack.txt").read_text() == "texture pack"
    assert os.path.islink(dest / "Load") or os.path.isjunction(dest / "Load")
    assert (dest / "Wii" / "sd.raw").read_bytes() == (template / "Wii" / "sd.raw").read_bytes()
    assert (dest / "Logs").is_dir() and not any((dest / "Logs").iterdir())
    remove_tree(dest, linked)
    assert not dest.exists()
    # the template's linked content survives the removal
    assert (template / "Load" / "Textures" / "pack.txt").read_text() == "texture pack"


def test_fast_copy_file_large(tmp_path):
    src = tmp_path / "big.bin"
    data = os.urandom(1 << 20) * 8
    src.write_bytes(data)
    _platform.fast_copy_file(src, tmp_path / "copy.bin")
    assert (tmp_path / "copy.bin").read_bytes() == data


# --------------------------------------------------------------------------- config


def test_config_overrides_written(make_instance):
    cfg = InstanceConfig(cpu_thread=False, video_backend="OGL", standard_controllers=(0, 3),
                         emulation_speed=2.0, dolphin_ini={"Core": {"Fastmem": False}},
                         gcpad_ini={"GCPad4": {"Buttons/A": "`Button 0`"}})
    inst = make_instance("cfg", config=cfg)
    d = inst.create()
    ini = IniFile.load(d / "Config" / "Dolphin.ini")
    assert ini.get("Core", "CPUThread") == "False"
    assert ini.get("Core", "GFXBackend") == "OGL"
    assert [ini.get("Core", f"SIDevice{p}") for p in range(4)] == ["6", "0", "0", "6"]
    assert ini.get("Core", "EmulationSpeed") == "2.0"
    assert ini.get("Core", "Fastmem") == "False"
    assert ini.get("Core", "WiiSDCardEnableFolderSync") == "False"   # Load is linked
    assert ini.get("NetPlay", "TraversalChoice") == "direct"
    assert ini.get("NetPlay", "UseUPNP") == "False"
    # sound off and no background input, always
    assert ini.get("DSP", "Muted") == "True"
    assert ini.get("DSP", "Volume") == "50"                          # untouched
    assert ini.get("Input", "BackgroundInput") == "False"
    pad = IniFile.load(d / "Config" / "GCPadNew.ini")
    assert pad.get("GCPad1", "Device") == "Harness/0/None"
    assert pad.get("GCPad1", "Buttons/A") is None                    # physical bindings gone
    assert pad.get("GCPad4", "Buttons/A") == "`Button 0`"
    # the template itself is untouched
    tpl = IniFile.load(inst.template / "Config" / "Dolphin.ini")
    assert tpl.get("DSP", "Muted") == "False" and tpl.get("Core", "SIDevice0") == "12"


def test_command_line(make_instance):
    inst = make_instance("args", config=InstanceConfig(config_args=["Dolphin.Core.Fastmem=False"]))
    inst.create()
    inst.port = 5555
    args = inst.build_args()
    assert args[args.index("--harness-port") + 1] == "5555"
    assert args[args.index("-u") + 1] == str(inst.user_dir)
    assert args[args.index("-p") + 1] == "headless"
    assert args[args.index("-v") + 1] == "Null"
    assert args[args.index("-e") + 1].endswith("Project+ Offline Launcher.dol")
    cs = [args[i + 1] for i, a in enumerate(args) if a == "-C"]
    assert cs == ["Dolphin.DSP.Muted=True", "Dolphin.Core.Fastmem=False"]


def test_audio_opt_in(make_instance):
    inst = make_instance("audio", audio=True, boot=None)
    inst.create()
    inst.port = 1
    assert IniFile.load(inst.user_dir / "Config" / "Dolphin.ini").get("DSP", "Muted") == "False"
    args = inst.build_args()
    assert "Dolphin.DSP.Muted=True" not in args
    assert "-e" not in args                                           # boot=None


def test_audio_env_reaches_process(make_instance):
    with make_instance("loud", audio=True) as inst:
        inst.wait_for_log(r"PPR_HARNESS_AUDIO=1$", timeout=5)
    with make_instance("quiet") as inst:
        inst.wait_for_log(r"PPR_HARNESS_AUDIO=$", timeout=5)


def test_instance_dirs_are_numbered(make_instance, instances_root):
    a, b = make_instance("same"), make_instance("same")
    assert a.create().name == "same-0"
    assert b.create().name == "same-1"
    assert make_instance("weird name::[x]").create().name == "weird_name_x-0"


# --------------------------------------------------------------------------- lifecycle


def test_full_lifecycle_graceful_quit(make_instance):
    inst = make_instance("life")
    with inst:
        c = inst.client
        assert c is not None and c.status().state == "running"
        c.wait_frame(5)
        d = inst.user_dir
        assert inst.is_running()
        # stdout / stderr captured to files
        deadline = time.monotonic() + 5
        while "harness on" not in inst.stdout_text() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert "fake-dolphin: harness on" in inst.stdout_text()
        assert "this is stderr" in inst.stderr_text()
        # the -C mute override reached the process
        inst.wait_for_log(r"Config override: Dolphin\.DSP\.Muted=True", timeout=5)
        # the template's stale log was not copied
        assert "stale log" not in inst.log.read()
    assert inst.exit_code == 0 and not inst.killed
    assert not d.exists()                                           # removed on success


def test_log_mark_and_wait_for_log(make_instance):
    with make_instance("logs") as inst:
        off = inst.mark("step one")
        assert off > 0
        assert inst.log.read(off).find("step one") == -1             # since= excludes it
        inst.client.log_mark("hello 42")
        m = inst.wait_for_log(r"hello (\d+)", since=off, timeout=5)
        assert m.group(1) == "42"
        with pytest.raises(InstanceError, match="not found"):
            inst.wait_for_log("never printed", timeout=0.3)


def test_kept_on_exception(make_instance):
    inst = make_instance("boom")
    with pytest.raises(RuntimeError):
        with inst:
            raise RuntimeError("test failure")
    assert inst.user_dir.exists()
    assert inst.exit_code == 0 and not inst.is_running()            # process still cleaned up
    remove_tree(inst.user_dir, inst.linked_dirs)


def test_keep_option(make_instance):
    inst = make_instance("keepme", keep=True)
    with inst:
        pass
    assert inst.user_dir.exists()
    remove_tree(inst.user_dir, inst.linked_dirs)


def test_keep_env(make_instance, monkeypatch):
    monkeypatch.setenv("PPHARNESS_KEEP", "1")
    inst = make_instance("keepenv")
    assert inst.keep


def test_kill_when_quit_is_ignored(make_instance):
    inst = make_instance("stubborn", env={"PPH_FAKE": "ignore_quit"}, quit_timeout=1.0)
    t0 = time.monotonic()
    with inst:
        pass
    assert inst.killed
    assert not inst.is_running()
    assert time.monotonic() - t0 < 15


def test_crash_on_start_reports_output(make_instance):
    inst = make_instance("crash", env={"PPH_FAKE": "crash"})
    with pytest.raises(InstanceError) as ei:
        with inst:
            pass
    msg = str(ei.value)
    assert "process exited" in msg
    assert "simulated crash" in msg                                 # stderr tail included
    assert inst.process.returncode == 3
    assert inst.user_dir.exists()                                   # kept for debugging
    remove_tree(inst.user_dir, inst.linked_dirs)


def test_old_build_without_harness(make_instance):
    inst = make_instance("old", env={"PPH_FAKE": "no_harness"})
    with pytest.raises(InstanceError, match="process exited"):
        inst.start()
    assert "harness-port" in inst.stderr_text()
    inst.mark_failed()


def test_connect_retries_while_booting(make_instance):
    with make_instance("slow", env={"PPH_FAKE": "listen_delay=1.5"}) as inst:
        assert inst.client.ping() == 1


def test_connect_timeout(make_instance):
    inst = make_instance("never", env={"PPH_FAKE": "listen_delay=30"}, connect_timeout=1.0)
    with pytest.raises(InstanceError, match="no harness server"):
        inst.start()
    inst.mark_failed()
    inst.stop(timeout=1)
    assert not inst.is_running()


def test_missing_exe(make_instance, tmp_path):
    inst = make_instance("noexe", exe=tmp_path / "nope.exe")
    with pytest.raises(InstanceError, match="not found"):
        inst.launch()


def test_stop_is_idempotent(make_instance):
    inst = make_instance("idem")
    inst.start()
    assert inst.stop() == 0
    assert inst.stop() == 0
    inst.cleanup()
    assert not inst.user_dir.exists()


def test_child_dies_with_parent(template, instances_root, fake_exe, tmp_path):
    """If the Python driver is killed, its Dolphins must not linger."""
    script = tmp_path / "driver.py"
    script.write_text(
        "import sys, time\n"
        f"sys.path.insert(0, {str(Path(__file__).resolve().parents[1])!r})\n"
        "from ppharness.instance import DolphinInstance\n"
        f"inst = DolphinInstance('orphan', exe={fake_exe!r}, template={str(template)!r}, "
        f"instances_root={str(instances_root)!r})\n"
        "inst.start()\n"
        "print(inst.process.pid, flush=True)\n"
        "time.sleep(60)\n")
    driver = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, text=True)
    try:
        child_pid = int(driver.stdout.readline())
        assert _platform.pid_alive(child_pid)
        driver.kill()
        driver.wait(10)
        deadline = time.monotonic() + 10
        while _platform.pid_alive(child_pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        if _platform.IS_MACOS:
            pytest.skip("no parent-death binding on macOS")
        assert not _platform.pid_alive(child_pid)
    finally:
        if driver.poll() is None:
            driver.kill()
    results = clean_instances(instances_root)
    assert results and all(a == "removed" for _, a in results)


def test_disk_space_check(make_instance, monkeypatch):
    import shutil as sh
    from collections import namedtuple
    Usage = namedtuple("Usage", "total used free")
    monkeypatch.setattr(sh, "disk_usage", lambda p: Usage(10, 10, 1024))
    inst = make_instance("full")
    with pytest.raises(InstanceError, match="not enough disk space"):
        inst.create()


# --------------------------------------------------------------------------- log tail


def test_logtail_handles_missing_partial_and_truncation(tmp_path):
    p = tmp_path / "x.log"
    lt = LogTail(p)
    assert lt.read() == "" and lt.mark() == 0
    with pytest.raises(LogTimeoutError):
        lt.wait_for("x", timeout=0.1)
    p.write_text("abc\npartial line")
    assert lt.wait_for(r"partial (\w+)", timeout=1).group(1) == "line"
    off = lt.mark()
    with open(p, "a") as f:
        f.write(" more\nnew line\n")
    assert lt.wait_for("new line", since=off, timeout=1)
    p.write_text("fresh\n")                 # truncated: offsets reset
    assert lt.wait_for("fresh", since=off, timeout=1)
    reason = iter(["", "", "died"])
    with pytest.raises(LogTimeoutError, match="died"):
        lt.wait_for("never", timeout=5, abort=lambda: next(reason) or None)


def test_clean_skips_running(make_instance, instances_root):
    with make_instance("running"):
        results = clean_instances(instances_root)
        assert any("still running" in a for _, a in results)
