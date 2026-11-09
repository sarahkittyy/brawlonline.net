import { cleanup, fireEvent, render } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { PlayButtonsView } from "./play_buttons_view";

describe("PlayButtonsView", () => {
  afterEach(cleanup);

  it("plays the clicked queue and shows no player counts", () => {
    const onPlay = vi.fn();
    const { container, getByTestId } = render(<PlayButtonsView onPlay={onPlay} />);
    fireEvent.click(getByTestId("play-ranked"));
    fireEvent.click(getByTestId("play-unranked"));
    fireEvent.click(getByTestId("play-twos"));
    expect(onPlay.mock.calls).toEqual([["ranked"], ["unranked"], ["twos"]]);
    expect(container.textContent).toBe("Play unrankedPlay rankedPlay 2v2");
  });

  it("waits while a Play is handed to the game", () => {
    const onPlay = vi.fn();
    const { getByTestId } = render(<PlayButtonsView starting="unranked" onPlay={onPlay} />);
    expect((getByTestId("play-unranked") as HTMLButtonElement).disabled).toBe(true);
    expect((getByTestId("play-ranked") as HTMLButtonElement).disabled).toBe(true);
    expect((getByTestId("play-twos") as HTMLButtonElement).disabled).toBe(true);
  });
});
