"""Tests for mac_publish.py: the publish decision and the ES256 token. python -m unittest (from here)."""

import base64
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import mac_publish as mp


def b(version, status, run=1):
    return mp.Build(version=version, submission=f"sub-{version}", status=status, run_id=run, marker_id=run)


class PlanTest(unittest.TestCase):
    def names(self, builds):
        return sorted(x.version for x in builds)

    def test_waits_while_apple_works(self):
        p = mp.plan([b("0.1.50", "In Progress")], "0.1.49")
        self.assertIsNone(p.publish)
        self.assertEqual(self.names(p.wait), ["0.1.50"])
        self.assertEqual(p.drop, [])

    def test_publishes_the_newest_accepted(self):
        p = mp.plan([b("0.1.50", "Accepted", 1), b("0.1.52", "Accepted", 2), b("0.1.51", "Accepted", 3)], "0.1.49")
        self.assertEqual(p.publish.version, "0.1.52")
        self.assertEqual(self.names([x for x, _ in p.drop]), ["0.1.50", "0.1.51"])

    def test_a_late_old_build_never_replaces_a_newer_one(self):
        # 0.1.55 is live; Apple finally accepts 0.1.53.
        p = mp.plan([b("0.1.53", "Accepted")], "0.1.55")
        self.assertIsNone(p.publish)
        self.assertEqual([x.version for x, _ in p.drop], ["0.1.53"])

    def test_the_same_version_is_not_published_again(self):
        p = mp.plan([b("0.1.55", "Accepted")], "0.1.55")
        self.assertIsNone(p.publish)
        self.assertEqual([x.version for x, _ in p.drop], ["0.1.55"])

    def test_newer_builds_still_at_apple_keep_waiting(self):
        p = mp.plan([b("0.1.50", "Accepted"), b("0.1.51", "In Progress"), b("0.1.49", "In Progress")], "0.1.48")
        self.assertEqual(p.publish.version, "0.1.50")
        self.assertEqual(self.names(p.wait), ["0.1.51"])
        self.assertEqual([x.version for x, _ in p.drop], ["0.1.49"])   # superseded by 0.1.50

    def test_rejected_builds_are_reported_and_dropped(self):
        p = mp.plan([b("0.1.50", "Invalid"), b("0.1.51", "Rejected")], "0.1.49")
        self.assertIsNone(p.publish)
        self.assertEqual(self.names(p.rejected), ["0.1.50", "0.1.51"])
        self.assertEqual(self.names([x for x, _ in p.drop]), ["0.1.50", "0.1.51"])

    def test_unknown_status_waits(self):
        p = mp.plan([b("0.1.50", None)], "0.1.49")
        self.assertEqual(self.names(p.wait), ["0.1.50"])

    def test_first_macos_release(self):
        p = mp.plan([b("0.1.50", "Accepted")], None)
        self.assertEqual(p.publish.version, "0.1.50")

    def test_versions_compare_as_numbers(self):
        p = mp.plan([b("0.1.100", "Accepted")], "0.1.99")
        self.assertEqual(p.publish.version, "0.1.100")


class TokenTest(unittest.TestCase):
    def test_es256_token_verifies(self):
        with tempfile.TemporaryDirectory() as tmp:
            key, pub = Path(tmp) / "k.p8", Path(tmp) / "k.pub"
            subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256",
                            "-out", str(key)], check=True, capture_output=True)
            subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-out", str(pub)],
                           check=True, capture_output=True)
            env = {"APPLE_API_KEY_P8": key.read_text(), "APPLE_API_KEY_ID": "KEY123", "APPLE_API_ISSUER": "iss-uuid"}
            old = {k: os.environ.get(k) for k in env}
            os.environ.update(env)
            try:
                token = mp.notary_token()
            finally:
                for k, v in old.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
            head, claims, sig = token.split(".")
            pad = lambda s: s + "=" * (-len(s) % 4)  # noqa: E731
            self.assertEqual(json.loads(base64.urlsafe_b64decode(pad(head)))["kid"], "KEY123")
            c = json.loads(base64.urlsafe_b64decode(pad(claims)))
            self.assertEqual((c["iss"], c["aud"]), ("iss-uuid", "appstoreconnect-v1"))
            raw = base64.urlsafe_b64decode(pad(sig))
            self.assertEqual(len(raw), 64)
            # r || s back to DER, then openssl verifies it against the public key.
            def der_int(x):
                x = x.lstrip(b"\0") or b"\0"
                if x[0] & 0x80:
                    x = b"\0" + x
                return b"\x02" + bytes([len(x)]) + x
            body = der_int(raw[:32]) + der_int(raw[32:])
            der = b"\x30" + bytes([len(body)]) + body
            (Path(tmp) / "sig").write_bytes(der)
            (Path(tmp) / "msg").write_bytes(f"{head}.{claims}".encode())
            out = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature",
                                  str(Path(tmp) / "sig"), str(Path(tmp) / "msg")], capture_output=True, text=True)
            self.assertIn("Verified OK", out.stdout)


if __name__ == "__main__":
    unittest.main()
