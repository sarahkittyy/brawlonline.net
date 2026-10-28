import { NatType } from "@common/types";
import { describe, expect, it } from "vitest";

import { natTypeOf } from "./network_diagnostics";

describe("natTypeOf", () => {
  const google = { server: "74.125.250.129", address: "109.55.79.131", port: 12113 };

  it("is normal with the same public address and port toward two servers", () => {
    expect(natTypeOf(google, { ...google, server: "162.159.207.0" })).toBe(NatType.NORMAL);
  });

  it("is symmetric with another port or another address toward the second server", () => {
    expect(natTypeOf(google, { ...google, server: "162.159.207.0", port: 12114 })).toBe(NatType.SYMMETRIC);
    expect(natTypeOf(google, { ...google, server: "162.159.207.0", address: "109.55.77.185" })).toBe(NatType.SYMMETRIC);
  });

  it("tells nothing from one server address (stun1 and stun2.l.google.com, bug report #169)", () => {
    expect(natTypeOf(google, google)).toBe(NatType.UNKNOWN);
  });
});
