import type { Ruleset } from "@common/rulesets";
import { defaultRuleset } from "@common/rulesets";
import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { useState } from "react";
import { afterEach, describe, expect, it } from "vitest";

import { RulesetEditor } from "./ruleset_editor";

let latest: Ruleset = defaultRuleset();

const Editor = ({ start }: { start: Ruleset }) => {
  const [r, setR] = useState(start);
  latest = r;
  return (
    <RulesetEditor
      value={r}
      onChange={(n) => {
        latest = n;
        setR(n);
      }}
    />
  );
};

describe("the ruleset editor's tabs", () => {
  afterEach(cleanup);

  it("shows the stages first, then items and special as tabs", () => {
    render(<Editor start={defaultRuleset()} />);
    expect(screen.getByText("Stage hazards (default)")).toBeTruthy();
    expect(screen.queryByTestId("special-section")).toBeNull();
    fireEvent.click(screen.getByTestId("ruleset-tab-items"));
    expect(screen.getByTestId("items-section")).toBeTruthy();
    expect(screen.queryByTestId("items-toggle")).toBeNull(); // open, without its own header
    fireEvent.click(screen.getByTestId("ruleset-tab-special"));
    expect(screen.getByTestId("special-section")).toBeTruthy();
    expect(screen.queryByText("Stage hazards (default)")).toBeNull();
  });

  it("turns special rows on and off, with the stamina type under stamina", () => {
    render(<Editor start={defaultRuleset()} />);
    fireEvent.click(screen.getByTestId("ruleset-tab-special"));
    const row = (key: string) => screen.getByTestId("special-section").querySelector(`[data-special="${key}"]`)!;
    const press = (key: string, label: string) =>
      fireEvent.click(Array.from(row(key).querySelectorAll("button")).find((b) => b.textContent === label)!);
    expect(screen.queryByText("Stamina type")).toBeNull();
    press("stamina", "Stamina");
    press("other", "All-Star");
    expect(latest.rules.special).toEqual({ stamina: "stamina", other: "all-star" });
    expect(screen.getByText(/Fight with a different character per stock!/)).toBeTruthy();
    expect(screen.getByText("Set knockback with blastzones.")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "3" }));
    expect(latest.rules.staminaType).toBe(3);
    // The type's own description (P+'s footer line) under the label.
    expect(screen.getByText("Normal knockback without blastzones.")).toBeTruthy();
    expect(screen.queryByText("Set knockback with blastzones.")).toBeNull();
    // Off again: the row is left out and the stamina type goes back to 1.
    press("stamina", "Off");
    expect(latest.rules.special).toEqual({ other: "all-star" });
    expect(latest.rules.staminaType).toBe(1);
    expect(screen.queryByText("Stamina type")).toBeNull();
  });

  it("makes stock control frame control with regen, in any style, and off with stamina", () => {
    const timed = defaultRuleset();
    timed.rules = { ...timed.rules, style: "time", timeMinutes: 2 };
    render(<Editor start={timed} />);
    const control = () => screen.getByText(/^(Stock|Frame) control$/).closest("div")!.parentElement!;
    expect(control().querySelector("input")!.disabled).toBe(true);
    fireEvent.click(screen.getByTestId("ruleset-tab-special"));
    fireEvent.click(
      Array.from(screen.getByTestId("special-section").querySelectorAll('[data-special="attack"] button')).find(
        (b) => b.textContent === "Regen",
      )!,
    );
    expect(screen.getByText("Frame control")).toBeTruthy();
    expect(control().querySelector("input")!.disabled).toBe(false);
  });
});
