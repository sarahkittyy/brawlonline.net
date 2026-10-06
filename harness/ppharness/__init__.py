"""ppharness: end-to-end test harness for the Project+ rollback Dolphin fork.

Modules:

* ``client``      - synchronous client for the JSON-lines control protocol
* ``instance``    - DolphinInstance: isolated user dir + process lifecycle + logs
* ``netsim``      - UDP impairment proxy (latency, jitter, loss, ...) + CLI
* ``session``     - two-player netplay setup and cross-instance state comparison
* ``mock_server`` - protocol implementation with fake memory/frames (for tests)
* ``fake_dolphin``- DolphinNoGUI stand-in that runs the mock server
"""

from .client import (
    FrameInfo,
    HarnessBusyError,
    HarnessClient,
    HarnessCommandError,
    HarnessConnectionError,
    HarnessError,
    HarnessTimeoutError,
    NetplayStatus,
    PadInput,
    Status,
    WaitTimeoutError,
)
from .instance import DolphinInstance, InstanceConfig, InstanceError
from .netsim import PRESETS, GilbertElliott, LinkProfile, NetProfile, NetSim, Spike

__version__ = "0.1.0"

__all__ = [
    "DolphinInstance", "FrameInfo", "GilbertElliott", "HarnessBusyError", "HarnessClient",
    "HarnessCommandError", "HarnessConnectionError", "HarnessError", "HarnessTimeoutError",
    "InstanceConfig", "InstanceError", "LinkProfile", "NetProfile", "NetSim", "NetplayStatus",
    "PRESETS", "PadInput", "Spike", "Status", "WaitTimeoutError",
]
