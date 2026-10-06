"""Tail a growing log file (Dolphin's ``Logs/dolphin.log``) and wait for patterns."""

from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Callable, Pattern


class LogTimeoutError(TimeoutError):
    def __init__(self, message: str, tail: str = ""):
        self.tail = tail
        super().__init__(message + (f"\n--- last log lines ---\n{tail}" if tail else ""))


class LogTail:
    """Read a log file incrementally. Offsets are byte offsets into the file.

    The file may not exist yet (Dolphin creates it during startup) and may be
    truncated/recreated; both are handled.
    """

    def __init__(self, path: str | os.PathLike[str], encoding: str = "utf-8"):
        self.path = Path(path)
        self.encoding = encoding

    def size(self) -> int:
        try:
            return self.path.stat().st_size
        except OSError:
            return 0

    def mark(self) -> int:
        """Current end of the file; pass it as ``since`` to only see newer lines."""
        return self.size()

    def read(self, since: int = 0) -> str:
        try:
            with open(self.path, "rb") as f:
                size = os.fstat(f.fileno()).st_size
                if since > size:  # truncated
                    since = 0
                f.seek(since)
                data = f.read()
        except OSError:
            return ""
        return data.decode(self.encoding, errors="replace")

    def lines(self, since: int = 0) -> list[str]:
        return self.read(since).splitlines()

    def tail(self, n: int = 40) -> str:
        return "\n".join(self.lines()[-n:])

    def wait_for(
        self,
        pattern: str | Pattern[str],
        timeout: float = 30.0,
        since: int = 0,
        interval: float = 0.05,
        abort: Callable[[], str | None] | None = None,
    ) -> re.Match[str]:
        """Wait until ``pattern`` (regex, searched per line) matches text after ``since``.

        ``abort`` is polled each round; if it returns a string (for example
        "process exited"), waiting stops with LogTimeoutError carrying that reason.
        """
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        pos = since
        pending = ""
        while True:
            try:
                size = self.path.stat().st_size
            except OSError:
                size = -1
            if size >= 0 and size < pos:
                pos, pending = 0, ""  # truncated or recreated
            if size > pos:
                with open(self.path, "rb") as f:
                    f.seek(pos)
                    data = f.read(size - pos)
                pos += len(data)
                text = pending + data.decode(self.encoding, errors="replace")
                # Search complete lines, and keep the trailing partial line for later
                # (but also search it, so a final line without '\n' is found).
                last_nl = text.rfind("\n")
                complete, pending = (text[:last_nl + 1], text[last_nl + 1:]) if last_nl >= 0 \
                    else ("", text)
                for line in complete.splitlines():
                    m = rx.search(line)
                    if m:
                        return m
            if pending:
                m = rx.search(pending)
                if m:
                    return m
            reason = abort() if abort else None
            if reason:
                raise LogTimeoutError(f"gave up waiting for /{rx.pattern}/ in {self.path}: {reason}",
                                      self.tail())
            if time.monotonic() >= deadline:
                raise LogTimeoutError(
                    f"/{rx.pattern}/ not found in {self.path} within {timeout:.1f}s", self.tail())
            time.sleep(interval)
