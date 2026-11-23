import { newRuleset } from "@common/rulesets";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { RulesetPreviewDialog } from "./ruleset_preview_dialog";

const fetchRuleset = vi.fn();
vi.mock("@/services", () => ({
  useServices: () => ({ backendService: { fetchRuleset, fetchRememberedRuleset: vi.fn() } }),
}));
vi.mock("@/lib/hooks/use_account", () => ({
  useAccount: (select: (s: { user: { uid: string } }) => unknown) => select({ user: { uid: "me" } }),
}));
vi.mock("@/lib/hooks/use_toasts", () => ({
  useToasts: () => ({ showSuccess: () => undefined, showError: () => undefined }),
}));

describe("the ruleset dialog with a room's custom rules", () => {
  it("shows them under their title, without looking anything up or offering a code", () => {
    const ruleset = { ...newRuleset(), name: "Custom", rules: { ...newRuleset().rules, stocks: 7, teamAttack: false } };
    render(
      <QueryClientProvider client={new QueryClient()}>
        <RulesetPreviewDialog open={true} custom={{ title: "Custom", ruleset }} onClose={() => undefined} />
      </QueryClientProvider>,
    );
    const dialog = screen.getByTestId("ruleset-preview-dialog");
    expect(dialog.textContent).toContain("Custom");
    const rows = Array.from(dialog.querySelectorAll("dl > div")).map(
      (d) => `${d.querySelector("dt")?.textContent}: ${d.querySelector("dd")?.textContent}`,
    );
    expect(rows).toContain("Stocks: 7");
    expect(rows.find((r) => r.startsWith("Team attack"))).toMatch(/Off/);
    expect(fetchRuleset).not.toHaveBeenCalled();
    expect(screen.queryByTestId("preview-use")).toBeNull();
  });
});
