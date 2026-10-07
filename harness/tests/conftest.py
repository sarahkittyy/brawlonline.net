"""Shared fixtures.

Options:
  --keep             keep every instance dir (also PPHARNESS_KEEP=1)
  --dolphin-exe PATH DolphinNoGUI to use for @pytest.mark.dolphin tests
  --no-dolphin       skip @pytest.mark.dolphin tests without probing
  --no-gpu           skip @pytest.mark.gpu tests (real video backend) without probing
  --video NAME       video backend for gpu tests (default: D3D11 on Windows, else Vulkan;
                     also PPHARNESS_VIDEO)
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

from ppharness import fake_dolphin, paths
from ppharness.instance import DolphinInstance, probe_harness_support


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--keep", action="store_true", help="keep instance dirs")
    parser.addoption("--dolphin-exe", default=None, help="DolphinNoGUI for dolphin tests")
    parser.addoption("--no-dolphin", action="store_true", help="skip dolphin tests")
    parser.addoption("--no-gpu", action="store_true", help="skip gpu (real video backend) tests")
    parser.addoption("--video", default=None, help="video backend for gpu tests")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: Any) -> Iterator[None]:
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


def _test_failed(request: pytest.FixtureRequest) -> bool:
    for when in ("setup", "call"):
        rep = getattr(request.node, f"rep_{when}", None)
        if rep is not None and rep.failed:
            return True
    return False


# --------------------------------------------------------------------------- fake template


@pytest.fixture
def template(tmp_path: Path) -> Path:
    """A tiny Dolphin user dir shaped like run/template-user."""
    t = tmp_path / "template"
    (t / "Config").mkdir(parents=True)
    (t / "Config" / "Dolphin.ini").write_text(
        "[Core]\nSIDevice0 = 12\nSIDevice1 = 12\nSIDevice2 = 6\nCPUThread = True\n"
        "[DSP]\nVolume = 50\nMuted = False\n[Input]\nBackgroundInput = True\n"
        "[NetPlay]\nTraversalChoice = traversal\nUseUPNP = True\n")
    (t / "Config" / "GCPadNew.ini").write_text(
        "[GCPad1]\nDevice = DInput/0/Keyboard Mouse\nButtons/A = `X`\n[GCPad2]\nDevice = DInput/0/Keyboard Mouse\n")
    (t / "Wii").mkdir()
    (t / "Wii" / "sd.raw").write_bytes(os.urandom(256 * 1024))
    (t / "Load" / "Textures").mkdir(parents=True)
    (t / "Load" / "Textures" / "pack.txt").write_text("texture pack")
    (t / "Logs").mkdir()
    (t / "Logs" / "dolphin.log").write_text("stale log from the template\n")
    (t / "Launcher").mkdir()
    (t / "Launcher" / "Project+ Offline Launcher.dol").write_bytes(b"\0" * 16)
    (t / "Launcher" / "Project+ Netplay Launcher.dol").write_bytes(b"\0" * 16)
    return t


@pytest.fixture
def instances_root(tmp_path: Path) -> Path:
    return tmp_path / "instances"


@pytest.fixture
def fake_exe() -> list[str]:
    return fake_dolphin.command()


@pytest.fixture
def make_instance(request: pytest.FixtureRequest, template: Path, instances_root: Path,
                  fake_exe: list[str]) -> Iterator[Callable[..., DolphinInstance]]:
    """Factory for fake-Dolphin instances; cleans up (keeping dirs if the test failed)."""
    made: list[DolphinInstance] = []
    keep = request.config.getoption("--keep")

    def factory(name: str = "t", **kw: Any) -> DolphinInstance:
        kw.setdefault("exe", fake_exe)
        kw.setdefault("template", template)
        kw.setdefault("instances_root", instances_root)
        kw.setdefault("connect_timeout", 20.0)
        kw.setdefault("keep", keep)
        inst = DolphinInstance(name, **kw)
        made.append(inst)
        return inst

    yield factory
    failed = _test_failed(request)
    for inst in made:
        if failed:
            inst.mark_failed()
        inst.cleanup(not failed)


# --------------------------------------------------------------------------- real Dolphin


_probe_result: tuple[bool, str] | None = None


@pytest.fixture(scope="session")
def dolphin_exe(request: pytest.FixtureRequest) -> Path:
    global _probe_result
    if request.config.getoption("--no-dolphin"):
        pytest.skip("--no-dolphin")
    exe = Path(request.config.getoption("--dolphin-exe") or paths.dolphin_nogui())
    if not exe.exists():
        pytest.skip(f"Dolphin binary not found: {exe}")
    if not paths.template_user_dir().is_dir():
        pytest.skip(f"template user dir not found: {paths.template_user_dir()}")
    if _probe_result is None:
        _probe_result = probe_harness_support(exe, timeout=20.0)
    ok, detail = _probe_result
    if not ok:
        pytest.skip(f"{exe} has no harness support: {detail}")
    return exe


@pytest.fixture
def dolphin(request: pytest.FixtureRequest, dolphin_exe: Path) -> Iterator[Callable[..., DolphinInstance]]:
    """Factory for real Dolphin instances under run/instances/<test-name>-<n>."""
    made: list[DolphinInstance] = []
    keep = request.config.getoption("--keep")

    # PPHARNESS_INSTANCE_PREFIX: prepended to every instance name, so that concurrent runs on one
    # machine have their own instance dirs (and `ppharness clean --prefix` only touches theirs).
    prefix = os.environ.get("PPHARNESS_INSTANCE_PREFIX", "")

    def factory(name: str | None = None, **kw: Any) -> DolphinInstance:
        kw.setdefault("exe", dolphin_exe)
        kw.setdefault("keep", keep)
        inst = DolphinInstance(prefix + (name or request.node.name), **kw)
        made.append(inst)
        return inst

    yield factory
    failed = _test_failed(request)
    for inst in made:
        if failed:
            inst.mark_failed()
        inst.cleanup(not failed)

_gpu_probe: dict[str, tuple[bool, str]] = {}


@pytest.fixture(scope="session")
def gpu_backend(request: pytest.FixtureRequest, dolphin_exe: Path) -> str:
    """A real video backend that works here (boots and takes a screenshot), or skip.

    D3D11 on Windows, Vulkan elsewhere; --video or PPHARNESS_VIDEO picks another. Probed once."""
    if request.config.getoption("--no-gpu"):
        pytest.skip("--no-gpu")
    import sys
    import tempfile

    from ppharness.instance import InstanceConfig

    backend = (request.config.getoption("--video") or os.environ.get("PPHARNESS_VIDEO")
               or ("D3D11" if sys.platform == "win32" else "Vulkan"))
    if backend not in _gpu_probe:
        try:
            with DolphinInstance("gpu-probe", exe=dolphin_exe, connect_timeout=60,
                                 config=InstanceConfig(video_backend=backend, cpu_thread=False)) as inst:
                inst.client.wait_state("running", timeout=60)
                inst.client.wait_frame(120, timeout_ms=60000)
                with tempfile.TemporaryDirectory() as tmp:
                    shot = inst.client.screenshot(str(Path(tmp) / "probe.png"))
                _gpu_probe[backend] = (shot.width > 0, f"{shot.width}x{shot.height}")
        except Exception as e:  # noqa: BLE001 - any failure means "no usable GPU backend here"
            _gpu_probe[backend] = (False, f"{type(e).__name__}: {e}")
    ok, detail = _gpu_probe[backend]
    if not ok:
        pytest.skip(f"video backend {backend} not usable here: {detail}")
    return backend
