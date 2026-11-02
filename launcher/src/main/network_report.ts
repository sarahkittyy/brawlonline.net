import type { PortMapping } from "@common/types";
import { NatType, Presence } from "@common/types";

export type NetworkDiagnostics = {
  address: string;
  cgnat: Presence;
  natType: NatType;
  portMapping: PortMapping;
};

/** How long a bug report waits for the network check before it goes without it. */
export const BUG_REPORT_NETWORK_TIMEOUT_MS = 12_000;

/**
 * `diagnostics` (Help > Network Diagnostics' check) as network.txt of a bug report, so a report
 * about matchmaking says what kind of network the player is on. `null` or a part of it: what the
 * check had found when it ran out of time; `error`: it failed.
 */
export function networkReportText(diagnostics: Partial<NetworkDiagnostics> | null, error?: unknown): string {
  const lines = ["Network check (the launcher's Network Diagnostics, run when the report was sent)", ""];
  if (error !== undefined) {
    lines.push(`Failed: ${error instanceof Error ? error.message : String(error)}`);
    return lines.join("\n") + "\n";
  }
  const timedOut = `did not finish within ${BUG_REPORT_NETWORK_TIMEOUT_MS / 1000} s`;
  if (diagnostics === null || Object.keys(diagnostics).length === 0) {
    lines.push(`Did not finish within ${BUG_REPORT_NETWORK_TIMEOUT_MS / 1000} s`);
    return lines.join("\n") + "\n";
  }
  // A check that ran past the timeout (often the traceroute) still writes what it had found.
  const { address, natType, cgnat, portMapping } = diagnostics;
  lines.push(`Public address: ${address === undefined ? timedOut : address || "unknown (STUN failed)"}`);
  lines.push(`NAT type: ${natType === undefined ? timedOut : natTypeText(natType)}`);
  lines.push(`Carrier-grade NAT: ${cgnat === undefined ? timedOut : cgnatText(cgnat)}`);
  lines.push(`UPnP: ${portMapping === undefined ? timedOut : presenceText(portMapping.upnp)}`);
  lines.push(`NAT-PMP: ${portMapping === undefined ? timedOut : presenceText(portMapping.natpmp)}`);
  return lines.join("\n") + "\n";
}

function natTypeText(natType: NatType): string {
  switch (natType) {
    case NatType.NORMAL:
      return "normal (the same public address and port toward two STUN servers)";
    case NatType.SYMMETRIC:
      return "symmetric (a new public address or port per destination: hole punching fails against strict routers)";
    case NatType.FAILED:
      return "unknown (the STUN check failed: UDP may be blocked)";
    case NatType.UNKNOWN:
      return "unknown (both STUN servers had one address)";
    default:
      return "unknown";
  }
}

function cgnatText(cgnat: Presence): string {
  switch (cgnat) {
    case Presence.PRESENT:
      return "likely (more than one hop to the public address: another NAT above the router)";
    case Presence.ABSENT:
      return "no (the public address is the next hop)";
    case Presence.FAILED:
      return "unknown (the traceroute failed)";
    default:
      return "unknown";
  }
}

function presenceText(presence: Presence): string {
  switch (presence) {
    case Presence.PRESENT:
      return "the router answers";
    case Presence.ABSENT:
      return "no answer";
    case Presence.FAILED:
      return "check failed";
    default:
      return "unknown";
  }
}
