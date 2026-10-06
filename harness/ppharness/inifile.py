"""Minimal, order-preserving editor for Dolphin INI files.

Dolphin's IniFile is ``[Section]`` + ``Key = Value`` lines with case-insensitive keys.
configparser would reformat or lowercase things, so we edit lines in place instead:
existing keys are replaced where they are, new keys go to the end of their section,
and unknown lines (comments, blank lines) are left alone.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Iterable, Mapping


def format_value(v: Any) -> str:
    if isinstance(v, bool):
        return "True" if v else "False"
    if isinstance(v, float):
        return repr(v)
    return str(v)


class IniFile:
    def __init__(self, lines: Iterable[str] = ()):
        self.lines: list[str] = [ln.rstrip("\r\n") for ln in lines]

    @classmethod
    def load(cls, path: str | os.PathLike[str]) -> "IniFile":
        p = Path(path)
        if not p.exists():
            return cls()
        return cls(p.read_text(encoding="utf-8", errors="replace").splitlines())

    def save(self, path: str | os.PathLike[str]) -> None:
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("\n".join(self.lines) + "\n", encoding="utf-8")

    # ------------------------------------------------------------------ parsing

    @staticmethod
    def _section_name(line: str) -> str | None:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            return s[1:-1].strip()
        return None

    @staticmethod
    def _key(line: str) -> str | None:
        s = line.strip()
        if not s or s[0] in "#;[" or "=" not in s:
            return None
        return s.split("=", 1)[0].strip()

    def _section_bounds(self, section: str) -> tuple[int, int] | None:
        """(header index, end index exclusive) of the first matching section."""
        start = None
        for i, line in enumerate(self.lines):
            name = self._section_name(line)
            if name is None:
                continue
            if start is not None:
                return start, i
            if name.lower() == section.lower():
                start = i
        if start is not None:
            return start, len(self.lines)
        return None

    def sections(self) -> list[str]:
        return [n for n in (self._section_name(ln) for ln in self.lines) if n is not None]

    def get(self, section: str, key: str, default: str | None = None) -> str | None:
        b = self._section_bounds(section)
        if b is None:
            return default
        for line in self.lines[b[0] + 1:b[1]]:
            k = self._key(line)
            if k is not None and k.lower() == key.lower():
                return line.split("=", 1)[1].strip()
        return default

    # ------------------------------------------------------------------ editing

    def set(self, section: str, key: str, value: Any) -> None:
        text = f"{key} = {format_value(value)}"
        b = self._section_bounds(section)
        if b is None:
            self.lines.append(f"[{section}]")
            self.lines.append(text)
            return
        start, end = b
        for i in range(start + 1, end):
            k = self._key(self.lines[i])
            if k is not None and k.lower() == key.lower():
                self.lines[i] = text
                return
        # Insert after the last non-blank line of the section.
        insert = end
        while insert > start + 1 and not self.lines[insert - 1].strip():
            insert -= 1
        self.lines.insert(insert, text)

    def remove(self, section: str, key: str) -> bool:
        b = self._section_bounds(section)
        if b is None:
            return False
        for i in range(b[0] + 1, b[1]):
            k = self._key(self.lines[i])
            if k is not None and k.lower() == key.lower():
                del self.lines[i]
                return True
        return False

    def replace_section(self, section: str, values: Mapping[str, Any]) -> None:
        """Drop every key of ``section`` and write ``values`` instead."""
        b = self._section_bounds(section)
        new = [f"[{section}]"] + [f"{k} = {format_value(v)}" for k, v in values.items()]
        if b is None:
            self.lines.extend(new)
        else:
            self.lines[b[0]:b[1]] = new

    def update(self, values: Mapping[str, Mapping[str, Any]]) -> None:
        for section, kv in values.items():
            for k, v in kv.items():
                self.set(section, k, v)


def edit_ini(path: str | os.PathLike[str], values: Mapping[str, Mapping[str, Any]]) -> None:
    ini = IniFile.load(path)
    ini.update(values)
    ini.save(path)
