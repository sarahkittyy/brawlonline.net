#!/usr/bin/env python3
"""Publishes macOS client builds once Apple's notary service accepts them, newest first, and never
an older version over a newer one.

The client workflow's macos job builds and signs the launcher, submits its DMG to Apple without
waiting (package-launcher-macos.sh, MAC_NOTARY_SUBMIT) and uploads two artifacts:

    launcher-mac   the DMG, the update zip, latest-mac.yml, blockmaps (what pp-release publishes)
    mac-notary     notary.json: {"id": <Apple submission ID>, "version": "0.1.N", "file": <DMG>}

This script (client-mac-publish.yml, every 10 minutes and after each client run, on sarahvps2)
looks at every pending mac-notary artifact and asks Apple about its submission:

    - In Progress: left alone, asked again next time. There is no time limit: a build waits as long
      as Apple takes (until its artifacts expire, 90 days).
    - Accepted, and newer than the published latest-mac.yml: the newest such build is published
      with pp-release, then checked on the site.
    - Rejected / Invalid: reported (with Apple's log URL) and dropped.
    - Not newer than what is published (or older than what this run publishes): dropped, so an old
      build that Apple answers late can never replace a newer one.

Dropped builds have both artifacts deleted. Runs are serialized (the workflow's concurrency group),
so the version check and the publish can't interleave with another run.

Needs: GH_TOKEN (actions: write), GITHUB_REPOSITORY, APPLE_API_KEY_P8 (the .p8's text),
APPLE_API_KEY_ID, APPLE_API_ISSUER; python3 and openssl. --dry-run only reports.
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

GITHUB_API = os.environ.get("GITHUB_API_URL", "https://api.github.com")
SITE = os.environ.get("SITE_URL", "https://brawlonline.net")
NOTARY_API = "https://appstoreconnect.apple.com/notary/v2"
MARKER = "mac-notary"
PAYLOAD = "launcher-mac"
PP_RELEASE = ["sudo", "-n", "/usr/local/sbin/pp-release", "client"]
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def log(msg: str) -> None:
    print(msg, flush=True)


def version_key(v: str | None) -> tuple[int, int, int] | None:
    m = VERSION_RE.match(v or "")
    return tuple(int(x) for x in m.groups()) if m else None  # type: ignore[return-value]


# --------------------------------------------------------------------------- the decision


@dataclass
class Build:
    version: str
    submission: str
    status: str | None     # Apple's status; None when it could not be asked this time
    run_id: int
    marker_id: int


@dataclass
class Plan:
    publish: Build | None
    drop: list[tuple[Build, str]]   # (build, why)
    wait: list[Build]
    rejected: list[Build]


def plan(builds: list[Build], published: str | None) -> Plan:
    """What to do with the pending builds, given the version latest-mac.yml names (None: none)."""
    pub = version_key(published)
    drop: list[tuple[Build, str]] = []
    rejected: list[Build] = []
    live: list[Build] = []
    for b in builds:
        k = version_key(b.version)
        if k is None:
            drop.append((b, f"bad version {b.version!r}"))
        elif b.status in ("Rejected", "Invalid"):
            rejected.append(b)
            drop.append((b, f"Apple: {b.status}"))
        elif pub is not None and k <= pub:
            drop.append((b, f"not newer than the published {published}"))
        else:
            live.append(b)
    accepted = [b for b in live if b.status == "Accepted"]
    best = max(accepted, key=lambda b: version_key(b.version)) if accepted else None
    wait: list[Build] = []
    for b in live:
        if b is best:
            continue
        if best is not None and version_key(b.version) < version_key(best.version):  # type: ignore[operator]
            drop.append((b, f"older than {best.version}, which is published now"))
        else:
            wait.append(b)
    return Plan(publish=best, drop=drop, wait=wait, rejected=rejected)


# --------------------------------------------------------------------------- GitHub


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):  # noqa: ANN002, ANN003
        return None


def gh(method: str, path: str, *, want: str = "json"):
    url = path if path.startswith("http") else f"{GITHUB_API}{path}"
    req = urllib.request.Request(url, method=method, headers={
        "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    })
    if want == "location":
        # The artifact zip endpoint redirects to blob storage, which refuses our Authorization header.
        opener = urllib.request.build_opener(_NoRedirect)
        try:
            opener.open(req, timeout=60)
        except urllib.error.HTTPError as e:
            if e.code in (301, 302, 303, 307, 308):
                return e.headers["Location"]
            raise
        raise RuntimeError(f"{url}: no redirect")
    with urllib.request.urlopen(req, timeout=60) as resp:
        body = resp.read()
    return json.loads(body) if want == "json" and body else body


def repo() -> str:
    return f"/repos/{os.environ['GITHUB_REPOSITORY']}"


def list_artifacts(name: str) -> list[dict]:
    out, page = [], 1
    while True:
        data = gh("GET", f"{repo()}/actions/artifacts?name={name}&per_page=100&page={page}")
        arts = data.get("artifacts", [])
        out += [a for a in arts if not a.get("expired")]
        if len(arts) < 100:
            return out
        page += 1


def run_artifact(run_id: int, name: str) -> dict | None:
    data = gh("GET", f"{repo()}/actions/runs/{run_id}/artifacts?per_page=100")
    return next((a for a in data.get("artifacts", []) if a["name"] == name and not a.get("expired")), None)


def download(artifact: dict) -> bytes:
    url = gh("GET", f"{repo()}/actions/artifacts/{artifact['id']}/zip", want="location")
    with urllib.request.urlopen(url, timeout=600) as resp:
        return resp.read()


def delete_run_artifacts(b: Build, dry: bool) -> None:
    for name in (MARKER, PAYLOAD):
        a = run_artifact(b.run_id, name)
        if a is None:
            continue
        if dry:
            log(f"  (dry run) would delete {name} of run {b.run_id}")
        else:
            gh("DELETE", f"{repo()}/actions/artifacts/{a['id']}", want="raw")
            log(f"  deleted {name} of run {b.run_id}")


# --------------------------------------------------------------------------- Apple's notary API


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _der_to_raw(sig: bytes) -> bytes:
    """An ECDSA signature from openssl (DER SEQUENCE of two INTEGERs) as JWS wants it (r || s)."""
    def read_int(buf: bytes, i: int) -> tuple[bytes, int]:
        assert buf[i] == 0x02, "INTEGER expected"
        n = buf[i + 1]
        return buf[i + 2:i + 2 + n], i + 2 + n
    assert sig[0] == 0x30, "SEQUENCE expected"
    i = 2 if sig[1] < 0x80 else 2 + (sig[1] & 0x7F)
    r, i = read_int(sig, i)
    s, _ = read_int(sig, i)
    return r.lstrip(b"\0").rjust(32, b"\0") + s.lstrip(b"\0").rjust(32, b"\0")


def notary_token() -> str:
    """App Store Connect API token (ES256 JWT, 15 minutes), signed with the .p8 through openssl."""
    now = int(time.time())
    header = {"alg": "ES256", "kid": os.environ["APPLE_API_KEY_ID"], "typ": "JWT"}
    claims = {"iss": os.environ["APPLE_API_ISSUER"], "iat": now, "exp": now + 900, "aud": "appstoreconnect-v1"}
    signing_input = f"{_b64url(json.dumps(header).encode())}.{_b64url(json.dumps(claims).encode())}".encode()
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "key.p8"
        key.write_text(os.environ["APPLE_API_KEY_P8"].strip() + "\n")
        key.chmod(0o600)
        der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(key), "-binary"],
                             input=signing_input, capture_output=True, check=True).stdout
    return f"{signing_input.decode()}.{_b64url(_der_to_raw(der))}"


def notary_get(path: str, token: str) -> dict:
    req = urllib.request.Request(f"{NOTARY_API}{path}", headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read())


def submission_status(sub: str, token: str) -> str | None:
    try:
        return notary_get(f"/submissions/{sub}", token)["data"]["attributes"]["status"]
    except (urllib.error.URLError, KeyError, ValueError) as e:
        log(f"  Apple: could not get the status of {sub} this time ({e})")
        return None


def submission_log_url(sub: str, token: str) -> str:
    try:
        return notary_get(f"/submissions/{sub}/logs", token)["data"]["attributes"]["developerLogUrl"]
    except (urllib.error.URLError, KeyError, ValueError) as e:
        return f"(no log URL: {e})"


# --------------------------------------------------------------------------- the site


def published_mac_version() -> str | None:
    url = f"{SITE}/updates/launcher/latest-mac.yml?nocache={int(time.time())}"
    # The site refuses Python's default user agent (403).
    req = urllib.request.Request(url, headers={"User-Agent": "brawlonline-ci/mac-publish"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            text = resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    m = re.search(r"^version:\s*['\"]?([^'\"\s]+)", text, re.M)
    return m.group(1) if m else None


# --------------------------------------------------------------------------- main


def read_marker(a: dict) -> dict | None:
    try:
        with zipfile.ZipFile(io.BytesIO(download(a))) as z:
            return json.loads(z.read("notary.json"))
    except (KeyError, ValueError, zipfile.BadZipFile, urllib.error.URLError) as e:
        log(f"  {MARKER} {a['id']} (run {a['workflow_run']['id']}): unreadable ({e})")
        return None


def publish(b: Build, work: Path, dry: bool) -> None:
    payload = run_artifact(b.run_id, PAYLOAD)
    if payload is None:
        raise RuntimeError(f"run {b.run_id} has no {PAYLOAD} artifact")
    dest = work / f"client-mac-{b.version}"
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(download(payload))) as z:
        z.extractall(dest)
    feed = (dest / "latest-mac.yml").read_text(errors="replace")
    named = re.search(r"^version:\s*['\"]?([^'\"\s]+)", feed, re.M)
    if not named or named.group(1) != b.version:
        raise RuntimeError(f"latest-mac.yml in run {b.run_id} names {named and named.group(1)}, not {b.version}")
    log(f"  files: {', '.join(sorted(p.name for p in dest.iterdir()))}")
    if dry:
        log(f"  (dry run) would run: {' '.join(PP_RELEASE)} {dest}")
        return
    subprocess.run([*PP_RELEASE, str(dest)], check=True)
    for attempt in range(10):
        got = published_mac_version()
        if got == b.version:
            log(f"  latest-mac.yml names {got}")
            break
        time.sleep(3)
    else:
        raise RuntimeError(f"after publishing, latest-mac.yml names {got}, not {b.version}")
    shutil.rmtree(dest, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=tempfile.gettempdir(), help="where to unpack a build to publish")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    work = Path(args.work)

    markers = list_artifacts(MARKER)
    if not markers:
        log("no macOS builds waiting for Apple")
        return 0
    token = notary_token()
    builds: list[Build] = []
    for a in markers:
        info = read_marker(a)
        if not info:
            continue
        status = submission_status(info["id"], token)
        builds.append(Build(version=str(info.get("version")), submission=info["id"], status=status,
                            run_id=a["workflow_run"]["id"], marker_id=a["id"]))
        log(f"{info.get('version')}: run {a['workflow_run']['id']}, submission {info['id']}: {status}")

    published = published_mac_version()
    log(f"published latest-mac.yml: {published}")
    p = plan(builds, published)

    failed = False
    for b in p.rejected:
        log(f"::error::macOS {b.version} (run {b.run_id}) was {b.status} by Apple: "
            f"{submission_log_url(b.submission, token)}")
        failed = True
    if p.publish:
        log(f"publishing macOS {p.publish.version} (run {p.publish.run_id})")
        try:
            publish(p.publish, work, args.dry_run)
            delete_run_artifacts(p.publish, args.dry_run)
        except (RuntimeError, subprocess.CalledProcessError, urllib.error.URLError) as e:
            log(f"::error::publishing macOS {p.publish.version} failed: {e}")
            # Nothing is dropped: the next run tries again.
            return 1
    for b, why in p.drop:
        log(f"dropping macOS {b.version} (run {b.run_id}): {why}")
        delete_run_artifacts(b, args.dry_run)
    for b in p.wait:
        log(f"waiting for Apple: macOS {b.version} (run {b.run_id}, {b.status or 'status unknown'})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
