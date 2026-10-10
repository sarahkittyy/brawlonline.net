"""A local online backend for tests: Postgres + ``accounts`` + ``mm`` from ``server/``.

Postgres, in this order:

1. ``PPHARNESS_PG_URL``: an admin URL of a server you run yourself (used as is; a fresh
   database is created in it and dropped afterwards);
2. a portable Postgres 16 in ``<root>/run/postgres-portable/pgsql`` (the EDB "binaries" zip
   extracted there): a throw-away cluster is ``initdb``-ed under the run dir, started on a free
   port, stopped and deleted afterwards. Nothing is installed or registered with the system;
3. Docker: ``docker compose up -d --wait`` in ``server/`` (Postgres on 127.0.0.1:54329). If this
   module started the container it runs ``docker compose down`` (never ``-v``) afterwards.

``accounts`` and ``mm`` are the debug binaries in ``server/target/debug`` (``cargo build
--workspace`` first). They run as plain processes with their working directory in the run dir,
so no ``server/.env`` is read; every setting comes from the environment given here.

Usage::

    with OnlineBackend() as be:
        alice = be.create_user("alice", "ALIC")   # sign-up, verify, code, user.json
        be.mm_port, be.accounts_url, alice.user_json, alice.connect_code
"""

from __future__ import annotations

import atexit
import json
import logging
import os
import secrets
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import _platform, paths
from .instance import find_free_port

log = logging.getLogger(__name__)


class BackendError(RuntimeError):
    pass


def server_dir() -> Path:
    return paths.workspace_root() / "server"


def server_binary(name: str) -> Path:
    return server_dir() / "target" / "debug" / _platform.exe_name(name)


def portable_pg_bin() -> Path:
    return paths.workspace_root() / "run" / "postgres-portable" / "pgsql" / "bin"


def _run(cmd: list[str], *, timeout: float = 120.0, env: dict[str, str] | None = None,
         cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env, cwd=cwd)
    if check and res.returncode != 0:
        raise BackendError(f"{' '.join(map(str, cmd))} failed ({res.returncode}): "
                           f"{res.stderr.strip() or res.stdout.strip()}")
    return res


@dataclass
class OnlineUser:
    email: str
    password: str
    display_name: str
    uid: str
    connect_code: str
    session_token: str
    user_json: dict[str, Any] = field(default_factory=dict)

    def write_user_json(self, user_dir: str | os.PathLike[str]) -> Path:
        """Writes ``<user_dir>/Online/user.json`` exactly as the launcher does
        (``JSON.stringify(playKey, null, 2)``)."""
        p = Path(user_dir) / "Online" / "user.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(self.user_json, indent=2), encoding="utf-8")
        return p


class OnlineBackend:
    def __init__(self, run_dir: str | os.PathLike[str] | None = None, *,
                 latest_version: str = "0.1.0", min_app_version: str | None = None,
                 ticket_ttl_secs: int | None = None,
                 rulesets_file: str | os.PathLike[str] | None = None, keep: bool = False):
        root = paths.workspace_root() / "run" / "backends"
        self.run_dir = Path(run_dir) if run_dir else root / f"be-{int(time.time())}-{secrets.token_hex(3)}"
        self.latest_version = latest_version
        self.min_app_version = min_app_version
        self.ticket_ttl_secs = ticket_ttl_secs
        # mm's per-mode rules (stage lists); None: the built-in server/config/rulesets.json.
        self.rulesets_file = Path(rulesets_file) if rulesets_file else None
        self.keep = keep
        self.pg_mode = ""
        self.admin_url = ""
        self.database_url = ""
        self.db_name = ""
        self.play_key_secret = secrets.token_hex(32)
        self.accounts_port = 0
        self.mm_port = 0
        # mm's status listener (the room list accounts serves): its default port is fixed, so two
        # backends at once (parallel test sessions) would collide.
        self.mm_status_port = 0
        self._pg_data: Path | None = None
        self._pg_port = 0
        self._compose_started = False
        self._procs: dict[str, subprocess.Popen[bytes]] = {}
        self._logs: dict[str, Any] = {}

    # ------------------------------------------------------------------ lifecycle

    @property
    def accounts_url(self) -> str:
        return f"http://127.0.0.1:{self.accounts_port}"

    def start(self) -> "OnlineBackend":
        for b in ("accounts", "mm", "admin"):
            if not server_binary(b).exists():
                raise BackendError(f"{server_binary(b)} not found; run `cargo build --workspace` "
                                   f"in {server_dir()}")
        self.run_dir.mkdir(parents=True, exist_ok=True)
        try:
            self._start_postgres()
            self._start_services()
        except BaseException:
            self.stop()
            raise
        return self

    def stop(self) -> None:
        for name, proc in list(self._procs.items()):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(10)
            self._procs.pop(name, None)
        for f in self._logs.values():
            f.close()
        self._logs.clear()
        self._stop_postgres()
        if not self.keep:
            shutil.rmtree(self.run_dir, ignore_errors=True)

    def __enter__(self) -> "OnlineBackend":
        return self.start()

    def __exit__(self, *exc: Any) -> None:
        if exc[0] is not None:
            self.keep = True
            log.error("backend logs kept in %s", self.run_dir)
        self.stop()

    def log_text(self, name: str) -> str:
        p = self.run_dir / f"{name}.log"
        return p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""

    # ------------------------------------------------------------------ postgres

    def _start_postgres(self) -> None:
        url = os.environ.get("PPHARNESS_PG_URL")
        if url:
            self.pg_mode = "external"
            self.admin_url = url
        elif (portable_pg_bin() / _platform.exe_name("pg_ctl")).exists():
            self.pg_mode = "portable"
            self._start_portable()
        else:
            self.pg_mode = "docker"
            self._start_docker()
        self.db_name = f"ppe2e_{int(time.time())}_{secrets.token_hex(3)}"
        # A freshly started portable server sometimes drops or times out the first connections
        # on Windows (seen with several clusters starting at once): retry a few times.
        for attempt in range(6):
            try:
                self._sql(self.admin_url, f"CREATE DATABASE {self.db_name}")
                break
            except BackendError as e:
                if attempt == 5 or "already exists" in str(e):
                    raise
                log.warning("CREATE DATABASE failed (%s); retrying", str(e).splitlines()[0][-120:])
                time.sleep(2.0)
        self.database_url = self.admin_url.rsplit("/", 1)[0] + "/" + self.db_name
        log.info("postgres (%s): %s", self.pg_mode, self.database_url)

    def _start_portable(self) -> None:
        bin_dir = portable_pg_bin()
        self._pg_data = self.run_dir / "pgdata"
        pwfile = self.run_dir / "pgpass.txt"
        pwfile.write_text("pp-dev-password\n")
        _run([str(bin_dir / "initdb"), "-D", str(self._pg_data), "-U", "pp", "-A", "scram-sha-256",
              f"--pwfile={pwfile}", "-E", "UTF8", "--no-locale"], timeout=180)
        pwfile.unlink()
        self._pg_port = find_free_port()
        # The server inherits pg_ctl's handles, so its output must not be a pipe we wait on.
        res = subprocess.run([str(bin_dir / "pg_ctl"), "-D", str(self._pg_data), "-l",
                              str(self.run_dir / "postgres.log"), "-o",
                              f"-p {self._pg_port} -h 127.0.0.1", "-w", "-t", "60", "start"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=90)
        atexit.register(self._stop_postgres)
        if res.returncode != 0:
            log_text = (self.run_dir / "postgres.log").read_text(errors="replace")
            raise BackendError(f"pg_ctl start failed:\n{log_text}")
        self.admin_url = f"postgres://pp:pp-dev-password@127.0.0.1:{self._pg_port}/postgres"

    def _start_docker(self) -> None:
        def compose(*a: str, timeout: float = 300.0, check: bool = True) -> subprocess.CompletedProcess[str]:
            return _run(["docker", "compose", *a], timeout=timeout, cwd=server_dir(), check=check)
        running = compose("ps", "--status", "running", "-q", check=False, timeout=60)
        if running.returncode != 0:
            raise BackendError("no Postgres: set PPHARNESS_PG_URL, extract a portable Postgres to "
                               f"{portable_pg_bin().parent}, or start Docker ({running.stderr.strip()})")
        if not running.stdout.strip():
            compose("up", "-d", "--wait")
            self._compose_started = True
        self.admin_url = "postgres://pp:pp-dev-password@127.0.0.1:54329/pp"

    def _sql(self, url: str, sql: str) -> None:
        if self.pg_mode == "portable":
            env = dict(os.environ, PGPASSWORD="pp-dev-password")
            _run([str(portable_pg_bin() / "psql"), "-h", "127.0.0.1", "-p", str(self._pg_port),
                  "-U", "pp", "-d", url.rsplit("/", 1)[1], "-v", "ON_ERROR_STOP=1", "-c", sql], env=env)
        elif self.pg_mode == "docker":
            _run(["docker", "compose", "exec", "-T", "postgres", "psql", "-U", "pp", "-d",
                  url.rsplit("/", 1)[1], "-v", "ON_ERROR_STOP=1", "-c", sql], cwd=server_dir())
        else:
            psql = shutil.which("psql") or str(portable_pg_bin() / _platform.exe_name("psql"))
            # The URL as -d, before the other options: psql on Windows stops parsing options at
            # the first non-option argument and ignores the rest (with only a warning).
            _run([psql, "-d", url, "-v", "ON_ERROR_STOP=1", "-c", sql])

    def _stop_postgres(self) -> None:
        if self.db_name and self.pg_mode in ("external", "docker"):
            try:
                self._sql(self.admin_url, f"DROP DATABASE IF EXISTS {self.db_name} WITH (FORCE)")
            except Exception as e:  # noqa: BLE001
                log.warning("could not drop %s: %s", self.db_name, e)
        self.db_name = ""
        if self.pg_mode == "portable" and self._pg_data is not None and self._pg_data.exists():
            _run([str(portable_pg_bin() / "pg_ctl"), "-D", str(self._pg_data), "-m", "fast", "-w",
                  "stop"], check=False, timeout=60)
            self._pg_data = None
        if self._compose_started:
            _run(["docker", "compose", "down"], cwd=server_dir(), check=False, timeout=300)
            self._compose_started = False

    # ------------------------------------------------------------------ services

    def _env(self) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MM_", "ACCOUNTS_", "MAIL", "RESEND_"))}
        env.update({
            "DATABASE_URL": self.database_url,
            "PLAY_KEY_SECRET": self.play_key_secret,
            "RUST_LOG": "info,sqlx=warn",
            "LATEST_VERSION": self.latest_version,
        })
        return env

    def _spawn(self, name: str, env: dict[str, str]) -> subprocess.Popen[bytes]:
        f = open(self.run_dir / f"{name}.log", "wb")  # noqa: SIM115 - closed in stop()
        self._logs[name] = f
        proc = subprocess.Popen([str(server_binary(name))], cwd=self.run_dir, env=env, stdout=f,
                                stderr=subprocess.STDOUT, **_platform.popen_kwargs())
        _platform.bind_to_parent(proc)
        self._procs[name] = proc
        return proc

    def _start_services(self) -> None:
        self.accounts_port = find_free_port()
        self.mm_status_port = find_free_port()
        env = self._env()
        env.update({
            "ACCOUNTS_LISTEN": f"127.0.0.1:{self.accounts_port}",
            "MM_STATUS_URL": f"http://127.0.0.1:{self.mm_status_port}/status",
            "PUBLIC_BASE_URL": f"http://127.0.0.1:{self.accounts_port}",
            "REQUIRE_EMAIL_VERIFICATION": "true",
            "MAILER": "file",
            "MAIL_FILE": str(self.run_dir / "mail.jsonl"),
            # Tests sign up several accounts from 127.0.0.1 within a minute.
            "DISABLE_RATE_LIMITS": "true",
            # Cheap hashing for tests (the production parameters take ~0.3 s per hash).
            "ARGON2_MEMORY_KIB": "8192",
            "ARGON2_ITERATIONS": "1",
        })
        accounts = self._spawn("accounts", env)
        deadline = time.monotonic() + 60
        while True:
            if accounts.poll() is not None:
                pg_log = self.run_dir / "postgres.log"
                pg_tail = "\n".join(pg_log.read_text(errors="replace").splitlines()[-15:]) \
                    if pg_log.exists() else ""
                raise BackendError(f"accounts exited ({accounts.returncode}):\n"
                                   f"{self.log_text('accounts')}\npostgres.log:\n{pg_tail}")
            try:
                if self._http("GET", "/healthz", raw=True) == "ok":
                    break
            except (OSError, BackendError):
                pass
            if time.monotonic() > deadline:
                raise BackendError(f"accounts did not answer:\n{self.log_text('accounts')}")
            time.sleep(0.2)

        self.mm_port = find_free_port(kind=socket.SOCK_DGRAM)
        env = self._env()
        env["MM_LISTEN"] = f"127.0.0.1:{self.mm_port}"
        env["MM_STATUS_LISTEN"] = f"127.0.0.1:{self.mm_status_port}"
        if self.min_app_version:
            env["MM_MIN_APP_VERSION"] = self.min_app_version
        if self.ticket_ttl_secs is not None:
            env["MM_TICKET_TTL_SECS"] = str(self.ticket_ttl_secs)
        if self.rulesets_file is not None:
            env["MM_RULESETS_FILE"] = str(self.rulesets_file.resolve())
        mm = self._spawn("mm", env)
        deadline = time.monotonic() + 30
        while "mm listening" not in self.log_text("mm"):
            if mm.poll() is not None:
                raise BackendError(f"mm exited ({mm.returncode}):\n{self.log_text('mm')}")
            if time.monotonic() > deadline:
                raise BackendError(f"mm did not start:\n{self.log_text('mm')}")
            time.sleep(0.1)
        log.info("accounts %s, mm udp 127.0.0.1:%d", self.accounts_url, self.mm_port)

    def alive(self) -> bool:
        return all(p.poll() is None for p in self._procs.values())

    # ------------------------------------------------------------------ admin + HTTP

    def admin(self, *args: str) -> str:
        env = self._env()
        env["PUBLIC_BASE_URL"] = self.accounts_url
        return _run([str(server_binary("admin")), *args], env=env, cwd=self.run_dir).stdout.strip()

    def _http(self, method: str, path: str, body: Any = None, token: str | None = None,
              raw: bool = False) -> Any:
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.accounts_url + path, data=data, method=method)
        if data is not None:
            req.add_header("content-type", "application/json")
        if token:
            req.add_header("authorization", f"Bearer {token}")
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                text = resp.read().decode()
        except urllib.error.HTTPError as e:
            raise BackendError(f"{method} {path}: HTTP {e.code}: {e.read().decode(errors='replace')}") from e
        return text if raw else (json.loads(text) if text else None)

    def create_user(self, display_name: str, code_start: str, *,
                    email: str | None = None, password: str = "a long test password") -> OnlineUser:
        """Sign-up (HTTP; open to everyone), verify (admin CLI), pick a code (HTTP) and fetch
        the launcher's user.json (HTTP)."""
        email = email or f"{display_name.lower()}-{secrets.token_hex(4)}@example.test"
        res = self._http("POST", "/v1/auth/signup", {"email": email, "password": password,
                                                     "displayName": display_name})
        token = res["sessionToken"]
        uid = res["user"]["uid"]
        self.admin("user", "verify-email", email)
        me = self._http("POST", "/v1/me/netplay", {"codeStart": code_start}, token=token)
        user_json = self._http("GET", "/v1/me/user-json", token=token)
        code = user_json.get("connectCode") or (me or {}).get("connectCode", "")
        return OnlineUser(email=email, password=password, display_name=display_name, uid=uid,
                          connect_code=code, session_token=token, user_json=user_json)
