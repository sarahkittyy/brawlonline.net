import { NatType, Presence } from "@common/types";
import { describe, expect, it } from "vitest";

import { networkReportText } from "./network_report";

describe("networkReportText", () => {
  it("names each finding", () => {
    const text = networkReportText({
      address: "203.0.113.7",
      cgnat: Presence.PRESENT,
      natType: NatType.SYMMETRIC,
      portMapping: { upnp: Presence.ABSENT, natpmp: Presence.PRESENT },
    });
    expect(text).toContain("Public address: 203.0.113.7\n");
    expect(text).toContain("NAT type: symmetric (");
    expect(text).toContain("Carrier-grade NAT: likely (");
    expect(text).toContain("UPnP: no answer\n");
    expect(text).toContain("NAT-PMP: the router answers\n");
  });

  it("says when STUN failed", () => {
    const text = networkReportText({
      address: "",
      cgnat: Presence.FAILED,
      natType: NatType.FAILED,
      portMapping: { upnp: Presence.FAILED, natpmp: Presence.FAILED },
    });
    expect(text).toContain("Public address: unknown (STUN failed)");
    expect(text).toContain("NAT type: unknown (the STUN check failed: UDP may be blocked)");
  });

  it("says when the check timed out or failed", () => {
    expect(networkReportText(null)).toContain("Did not finish within 12 s");
    expect(networkReportText(null, new Error("spawn tracert ENOENT"))).toContain("Failed: spawn tracert ENOENT");
  });
});
