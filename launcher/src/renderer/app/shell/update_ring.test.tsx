import { fireEvent, render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { LauncherUpdateAction } from "@/app/header/play_button/launcher_update";

import { LauncherUpdateRing, SetupRing } from "./update_ring";

const update = (over: Partial<LauncherUpdateAction> = {}): LauncherUpdateAction => ({
  currentVersion: "0.2.1",
  newVersion: "0.2.2",
  downloadProgress: undefined,
  disabled: false,
  onClick: vi.fn(),
  title: undefined,
  ...over,
});

const ring = (container: HTMLElement) => container.querySelector<HTMLButtonElement>('[data-testid="update-ring"]')!;

describe("LauncherUpdateRing", () => {
  it("fills as the update downloads and can't be clicked until it is downloaded", () => {
    const action = update({ downloadProgress: 0.45 });
    const { container } = render(<LauncherUpdateRing update={action} />);
    expect(ring(container).disabled).toBe(true);
    expect(ring(container).getAttribute("aria-label")).toBe("Update v0.2.1 → v0.2.2");
    const fill = container.querySelectorAll('[role="progressbar"]');
    expect(fill[fill.length - 1].getAttribute("aria-valuenow")).toBe("45");
  });

  it("installs on a click once downloaded", () => {
    const action = update();
    const { container } = render(<LauncherUpdateRing update={action} />);
    expect(container.querySelector('[role="progressbar"]')).toBeNull();
    fireEvent.click(ring(container));
    expect(action.onClick).toHaveBeenCalledTimes(1);
  });
});

describe("SetupRing", () => {
  it("names the step and how far along it is", () => {
    const { container } = render(<SetupRing phase="downloadProjectPlus" fillPercent={0.3} />);
    expect(container.querySelector('[data-testid="setup-ring"]')!.getAttribute("aria-label")).toBe(
      "Downloading Project+ 30%",
    );
  });
});
