"""A fake UPnP router (IGD) on this machine's LAN address, for Dolphin's online port mapping
(``Common::UPnP::MapUdpPort``) without touching the real router.

It answers SSDP searches (239.255.255.250:1900, joined on ``lan_ip``) with the URL of a device
description that has a WANIPConnection:1 service, and serves that service's SOAP actions:
GetStatusInfo (Connected), GetExternalIPAddress, AddPortMapping and DeletePortMapping. Every
mapping call is recorded in ``calls``. Nothing is forwarded: it only checks what Dolphin asks for.

    with FakeIGD(lan_ip) as igd:
        ...
        assert igd.calls[0].action == "AddPortMapping"
"""

from __future__ import annotations

import re
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

SSDP_ADDR = ("239.255.255.250", 1900)
WANIP = "urn:schemas-upnp-org:service:WANIPConnection:1"

_DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>ppharness fake IGD</friendlyName>
<UDN>uuid:{uuid}-0</UDN>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<UDN>uuid:{uuid}-1</UDN>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANCommonIFC1</serviceId>
<controlURL>/ctl/CmnIfCfg</controlURL><eventSubURL>/evt/CmnIfCfg</eventSubURL>
<SCPDURL>/WANCfg.xml</SCPDURL>
</service></serviceList>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<UDN>uuid:{uuid}-2</UDN>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL>
<SCPDURL>/WANIPCn.xml</SCPDURL>
</service></serviceList>
</device></deviceList>
</device></deviceList>
</device>
</root>
"""

_ENVELOPE = ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
             's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>{}</s:Body>'
             '</s:Envelope>\r\n')


@dataclass
class Call:
    action: str
    args: dict[str, str]
    error: int = 0  # the UPnP error code it was answered with, 0 for success
    at: float = field(default_factory=time.monotonic)


class FakeIGD:
    def __init__(self, lan_ip: str, *, external_ip: str = "11.22.33.44",
                 permanent_leases_only: bool = False):
        self.lan_ip = lan_ip
        self.external_ip = external_ip
        #: Answer a mapping with a lease with 725 OnlyPermanentLeasesSupported, as some routers do.
        self.permanent_leases_only = permanent_leases_only
        self.uuid = "8c1a3f2e-5d4b-4c3a-9e1f-" + format(int(time.time() * 1000) & 0xFFFFFFFFFFFF, "012x")
        self.calls: list[Call] = []
        self.searches = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._http: ThreadingHTTPServer | None = None
        self._ssdp: socket.socket | None = None
        self._threads: list[threading.Thread] = []

    @property
    def location(self) -> str:
        assert self._http is not None
        return f"http://{self.lan_ip}:{self._http.server_address[1]}/rootDesc.xml"

    @property
    def control_url(self) -> str:
        assert self._http is not None
        return f"http://{self.lan_ip}:{self._http.server_address[1]}/ctl/IPConn"

    def mapping_calls(self) -> list[Call]:
        with self._lock:
            return [c for c in self.calls if c.action in ("AddPortMapping", "DeletePortMapping")]

    # ------------------------------------------------------------------ lifecycle

    def start(self) -> "FakeIGD":
        igd = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args: Any) -> None:
                pass

            def do_GET(self) -> None:
                if self.path != "/rootDesc.xml":
                    self.send_error(404)
                    return
                self._reply(200, _DESC.format(uuid=igd.uuid))

            def do_POST(self) -> None:
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode(errors="replace")
                m = re.search(r"#(\w+)", self.headers.get("SOAPAction", ""))
                action = m.group(1) if m else ""
                args = dict(re.findall(r"<(New\w+)>([^<]*)</\1>", body))
                code, out = igd._handle(self.path, action, args)
                if code:
                    fault = ('<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>'
                             '<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
                             f'<errorCode>{code}</errorCode><errorDescription>error</errorDescription>'
                             '</UPnPError></detail></s:Fault>')
                    self._reply(500, _ENVELOPE.format(fault))
                    return
                inner = "".join(f"<{k}>{v}</{k}>" for k, v in out.items())
                self._reply(200, _ENVELOPE.format(
                    f'<u:{action}Response xmlns:u="{WANIP}">{inner}</u:{action}Response>'))

            def _reply(self, status: int, text: str) -> None:
                data = text.encode()
                self.send_response(status)
                self.send_header("Content-Type", 'text/xml; charset="utf-8"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

        self._http = ThreadingHTTPServer((self.lan_ip, 0), Handler)
        self._http.daemon_threads = True

        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", SSDP_ADDR[1]))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     struct.pack("4s4s", socket.inet_aton(SSDP_ADDR[0]), socket.inet_aton(self.lan_ip)))
        s.settimeout(0.2)
        self._ssdp = s

        for target in (self._http.serve_forever, self._ssdp_loop):
            t = threading.Thread(target=target, daemon=True, name="fake-igd")
            t.start()
            self._threads.append(t)
        return self

    def stop(self) -> None:
        self._stop.set()
        if self._http is not None:
            self._http.shutdown()
            self._http.server_close()
        for t in self._threads:
            t.join(5)
        if self._ssdp is not None:
            self._ssdp.close()

    def __enter__(self) -> "FakeIGD":
        return self.start()

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    # ------------------------------------------------------------------ protocol

    def _ssdp_loop(self) -> None:
        assert self._ssdp is not None
        while not self._stop.is_set():
            try:
                data, addr = self._ssdp.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            text = data.decode(errors="replace")
            if not text.startswith("M-SEARCH"):
                continue
            m = re.search(r"^ST:\s*(\S+)", text, re.MULTILINE | re.IGNORECASE)
            st = m.group(1) if m else ""
            if not (st in ("ssdp:all", "upnp:rootdevice") or "InternetGatewayDevice" in st
                    or "WANIPConnection:1" in st or "WANCommonInterfaceConfig:1" in st):
                continue
            with self._lock:
                self.searches += 1
            reply = (f"HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: {st}\r\n"
                     f"USN: uuid:{self.uuid}-0::{st}\r\nEXT:\r\nSERVER: ppharness/1.0 UPnP/1.1\r\n"
                     f"LOCATION: {self.location}\r\n\r\n")
            try:
                self._ssdp.sendto(reply.encode(), addr)
            except OSError:
                pass

    def _handle(self, path: str, action: str, args: dict[str, str]) -> tuple[int, dict[str, str]]:
        if path != "/ctl/IPConn":
            return 401, {}
        error = 0
        out: dict[str, str] = {}
        if action == "GetStatusInfo":
            out = {"NewConnectionStatus": "Connected", "NewLastConnectionError": "ERROR_NONE",
                   "NewUptime": "1000"}
        elif action == "GetExternalIPAddress":
            out = {"NewExternalIPAddress": self.external_ip}
        elif action == "AddPortMapping":
            if self.permanent_leases_only and args.get("NewLeaseDuration", "0") != "0":
                error = 725
        elif action == "DeletePortMapping":
            pass
        else:
            error = 401
        with self._lock:
            self.calls.append(Call(action, args, error))
        return error, out
