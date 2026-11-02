import type { PortMapping } from "@common/types";
import { NatType, Presence } from "@common/types";
import { createPmpClient, createUpnpClient } from "@xmcl/nat-api";
import { v4DefaultGateway } from "network-default-gateway";
import { lookup } from "node:dns/promises";
import { createServer, request } from "stun";

import type { NetworkDiagnostics } from "./network_report";
import { countHops } from "./traceroute";

// Two different machines: a router that gives every destination its own port shows it only toward
// two addresses. stun1 and stun2.l.google.com are one (74.125.250.129:19302), so every router
// passed as "normal" (bug reports #169 and #174, 2026-10-27: a port-changing router reported so).
const STUN_SERVER1 = { host: "stun.l.google.com", port: 19302 };
const STUN_SERVER2 = { host: "stun.cloudflare.com", port: 3478 };

/**
 * The network check. `found` gets each finding as soon as it is known, so a caller that stops
 * waiting (a bug report) still has the NAT type when the traceroute runs long.
 */
export async function getNetworkDiagnostics(found: Partial<NetworkDiagnostics> = {}): Promise<NetworkDiagnostics> {
  const natTypePromise = getNatType()
    .catch(() => ({ address: "", natType: NatType.FAILED }))
    .then((result) => Object.assign(found, result));
  const portMappingPromise = getPortMappingPresence()
    .catch(() => ({ upnp: Presence.FAILED, natpmp: Presence.FAILED }))
    .then((portMapping) => (found.portMapping = portMapping));
  const { address, natType } = await natTypePromise;

  // The traceroute (up to 9 s) runs while the router is asked for UPnP and NAT-PMP.
  const cgnatPromise = (
    address ? getCgnatPresence(address).catch(() => Presence.FAILED) : Promise.resolve(Presence.FAILED)
  ).then((cgnat) => (found.cgnat = cgnat));
  const [portMapping, cgnat] = await Promise.all([portMappingPromise, cgnatPromise]);
  return { address, cgnat, natType, portMapping };
}

async function getNatType(): Promise<{ address: string; natType: NatType }> {
  const [ip1, ip2] = await Promise.all(
    [STUN_SERVER1, STUN_SERVER2].map(async ({ host }) => (await lookup(host, { family: 4 })).address),
  );
  const stunServer = createServer({ type: "udp4" });
  try {
    const stunResponse1 = await request(`${ip1}:${STUN_SERVER1.port}`, { server: stunServer });
    const stunResponse2 = await request(`${ip2}:${STUN_SERVER2.port}`, { server: stunServer });
    const mapped1 = stunResponse1.getXorAddress();
    const mapped2 = stunResponse2.getXorAddress();
    return {
      address: mapped1.address,
      natType: natTypeOf({ server: ip1, ...mapped1 }, { server: ip2, ...mapped2 }),
    };
  } finally {
    stunServer.close();
  }
}

/**
 * The NAT type from one socket's public address and port as two STUN servers saw them: the same
 * toward both is a normal NAT, another address or port per destination a symmetric one (its
 * players are sent where the server saw it, so the strict routers' answers miss). Two answers from
 * one server address tell nothing.
 */
export function natTypeOf(
  a: { server: string; address: string; port: number },
  b: { server: string; address: string; port: number },
): NatType {
  if (a.server === b.server) {
    return NatType.UNKNOWN;
  }
  return a.address === b.address && a.port === b.port ? NatType.NORMAL : NatType.SYMMETRIC;
}

async function getPortMappingPresence(): Promise<PortMapping> {
  let upnpPresence = Presence.UNKNOWN;
  const upnpClient = await createUpnpClient();
  const upnpPromise = upnpClient
    .externalIp()
    .then(() => {
      upnpPresence = Presence.PRESENT;
    })
    .catch(() => {
      upnpPresence = Presence.ABSENT;
    })
    .finally(() => {
      upnpClient.destroy();
    });

  let natpmpPresence = Presence.UNKNOWN;
  const pmpClient = await createPmpClient((await v4DefaultGateway()).gateway);
  const pmpPromise = new Promise((resolve, reject) => {
    // library does not use a timeout for NAT-PMP, so we do it ourselves.
    const timeout = setTimeout(() => {
      reject("NAT-PMP timeout");
    }, 1800); // same as library UPnP timeout
    pmpClient
      .externalIp()
      .then(resolve)
      .catch(reject)
      .finally(() => {
        clearTimeout(timeout);
      });
  })
    .then(() => {
      natpmpPresence = Presence.PRESENT;
    })
    .catch(() => {
      natpmpPresence = Presence.ABSENT;
    })
    .finally(() => {
      pmpClient.close();
    });

  await Promise.all([upnpPromise, pmpPromise]);
  return { upnp: upnpPresence, natpmp: natpmpPresence } as PortMapping;
}

/** One hop to our own public address: the router has it. More: another NAT above the router (CGNAT). */
async function getCgnatPresence(address: string): Promise<Presence> {
  const { hops, finished } = await countHops(address);
  if (!finished && hops <= 1) {
    throw new Error("CGNAT timeout");
  }
  return hops === 1 ? Presence.ABSENT : Presence.PRESENT;
}
