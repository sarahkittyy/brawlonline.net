import type { PortMapping } from "@common/types";
import { NatType, Presence } from "@common/types";
import { createPmpClient, createUpnpClient } from "@xmcl/nat-api";
import { v4DefaultGateway } from "network-default-gateway";
import { createServer, request } from "stun";

import { countHops } from "./traceroute";

const STUN_SERVER_URL1 = "stun1.l.google.com:19302";
const STUN_SERVER_URL2 = "stun2.l.google.com:19302";

export async function getNetworkDiagnostics(): Promise<{
  address: string;
  cgnat: Presence;
  natType: NatType;
  portMapping: PortMapping;
}> {
  const natTypePromise = getNatType().catch(() => ({ address: "", natType: NatType.FAILED }));
  const portMappingPromise = getPortMappingPresence().catch(() => ({ upnp: Presence.FAILED, natpmp: Presence.FAILED }));
  const { address, natType } = await natTypePromise;
  const portMapping = await portMappingPromise;

  let cgnat = Presence.FAILED;
  if (address) {
    try {
      cgnat = await getCgnatPresence(address);
    } catch (err) {
      // just return what we have
    }
  }
  return { address, cgnat, natType, portMapping };
}

async function getNatType(): Promise<{ address: string; natType: NatType }> {
  const stunServer = createServer({ type: "udp4" });
  const stunResponse1 = await request(STUN_SERVER_URL1, { server: stunServer });
  const stunResponse2 = await request(STUN_SERVER_URL2, { server: stunServer });
  const address1 = stunResponse1.getXorAddress();
  const address2 = stunResponse2.getXorAddress();
  stunServer.close();
  return { address: address1.address, natType: address1.port === address2.port ? NatType.NORMAL : NatType.SYMMETRIC };
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
