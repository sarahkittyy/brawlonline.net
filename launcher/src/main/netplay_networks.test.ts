import { describe, expect, it } from "vitest";

import { listNetplayNetworks } from "./netplay_networks";

const info = (address: string, cidr: string | null, extra: Record<string, unknown> = {}) =>
  ({ address, cidr, family: "IPv4", internal: false, mac: "", netmask: "", ...extra } as any);

describe("listNetplayNetworks", () => {
  it("lists the IPv4 addresses with their prefix, not loopback, IPv6 or link-local ones", () => {
    const networks = listNetplayNetworks({
      Ethernet: [info("192.168.1.20", "192.168.1.20/24"), info("fe80::1", "fe80::1/64", { family: "IPv6" })],
      "ZeroTier One [8056c2e21c000001]": [info("10.147.17.5", "10.147.17.5/24")],
      "Loopback Pseudo-Interface 1": [info("127.0.0.1", "127.0.0.1/8", { internal: true })],
      "Ethernet 2": [info("169.254.10.3", "169.254.10.3/16")],
      Tailscale: [info("100.101.2.3", "100.101.2.3/32"), info("100.101.2.4", null)],
    });
    expect(networks).toEqual([
      { name: "Ethernet", cidr: "192.168.1.20/24" },
      { name: "ZeroTier One [8056c2e21c000001]", cidr: "10.147.17.5/24" },
      { name: "Tailscale", cidr: "100.101.2.3/32" },
    ]);
  });
});
