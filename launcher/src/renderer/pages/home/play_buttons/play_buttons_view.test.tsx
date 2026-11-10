import { cleanup, fireEvent, render } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { PlayButtonsView } from "./play_buttons_view";

describe("PlayButtonsView", () => {
  afterEach(cleanup);

  it("plays the clicked queue and shows counts under Play 2v2 only", () => {
    const onPlay = vi.fn();
    const { container, getByTestId } = render(<PlayButtonsView twosQueue={2} twosPlaying={8} onPlay={onPlay} />);
    fireEvent.click(getByTestId("play-ranked"));
    fireEvent.click(getByTestId("play-unranked"));
    fireEvent.click(getByTestId("play-twos"));
    expect(onPlay.mock.calls).toEqual([["ranked"], ["unranked"], ["twos"]]);
    expect(getByTestId("play-twos-count").textContent).toBe("2 in queue · 8 in game");
    expect(container.textContent).toBe("Play unrankedPlay rankedPlay 2v22 in queue · 8 in game");
  });

  it("shows only 2v2's queue when the server does not send the players in game", () => {
    const { getByTestId } = render(<PlayButtonsView twosQueue={3} onPlay={vi.fn()} />);
    expect(getByTestId("play-twos-count").textContent).toBe("3 in queue");
  });

  it("hides 2v2's counts while they are unknown", () => {
    const { getByTestId } = render(<PlayButtonsView onPlay={vi.fn()} />);
    expect(getByTestId("play-twos-count").style.visibility).toBe("hidden");
  });

  it("waits while a Play is handed to the game", () => {
    const onPlay = vi.fn();
    const { getByTestId } = render(<PlayButtonsView starting="unranked" onPlay={onPlay} />);
    expect((getByTestId("play-unranked") as HTMLButtonElement).disabled).toBe(true);
    expect((getByTestId("play-ranked") as HTMLButtonElement).disabled).toBe(true);
    expect((getByTestId("play-twos") as HTMLButtonElement).disabled).toBe(true);
  });
});
