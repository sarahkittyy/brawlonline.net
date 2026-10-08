import { render } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { PlayButton, setupPhaseLabel, UpdatingButton } from "./play_button";

describe("UpdatingButton", () => {
  it("says Updating, as Slippi's does, for a step without a name", () => {
    const { container } = render(<UpdatingButton fillPercent={0.5} />);
    expect(container.textContent).toBe("Updating");
    expect(container.querySelector("button")!.disabled).toBe(true);
  });

  it("names the running step and how far along it is", () => {
    const { container } = render(<UpdatingButton fillPercent={0.427} phase="prepareSdCard" />);
    expect(container.textContent).toBe("Preparing SD card42%");
    // The filled part is drawn over the face too (the game's frame hides the background).
    expect(container.querySelector('[data-testid="play-button-fill"]')).not.toBeNull();
    const { container: done } = render(<UpdatingButton fillPercent={1} phase="extractProjectPlus" />);
    expect(done.textContent).toBe("Extracting Project+100%");
  });

  it("draws no progress over the ready Play button", () => {
    const { container } = render(<PlayButton />);
    expect(container.textContent).toBe("Play");
    expect(container.querySelector('[data-testid="play-button-fill"]')).toBeNull();
  });

  it("has a plain label for every step", () => {
    expect(setupPhaseLabel("installDolphin")).toBe("Installing Dolphin");
    expect(setupPhaseLabel("copyUserFolder")).toBe("Copying files");
    expect(setupPhaseLabel("downloadProjectPlus")).toBe("Downloading Project+");
    expect(setupPhaseLabel("verifyProjectPlus")).toBe("Verifying Project+");
    expect(setupPhaseLabel("extractProjectPlus")).toBe("Extracting Project+");
    expect(setupPhaseLabel("prepareSdCard")).toBe("Preparing SD card");
    expect(setupPhaseLabel(undefined)).toBe("Updating");
  });
});
