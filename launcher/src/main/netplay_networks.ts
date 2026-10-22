import os from "os";

/** One of the PC's IPv4 addresses, for Settings' "Network for online games". */
export type NetplayNetwork = {
  /** The adapter's name ("ZeroTier One [8056c2e21c000001]", "Ethernet", "ztks54inm2"). */
  name: string;
  /** "a.b.c.d/prefix": the value the setting keeps (Dolphin's `[Online] LanIP`). */
  cidr: string;
};

/** The PC's IPv4 addresses other than loopback, in the OS's order. */
export function listNetplayNetworks(
  interfaces: NodeJS.Dict<os.NetworkInterfaceInfo[]> = os.networkInterfaces(),
): NetplayNetwork[] {
  const networks: NetplayNetwork[] = [];
  for (const [name, infos] of Object.entries(interfaces)) {
    for (const info of infos ?? []) {
      // Node 18.0-18.3 gave the family as a number.
      const ipv4 = info.family === "IPv4" || (info.family as unknown) === 4;
      if (ipv4 && !info.internal && info.cidr && !info.address.startsWith("169.254.")) {
        networks.push({ name, cidr: info.cidr });
      }
    }
  }
  return networks;
}
