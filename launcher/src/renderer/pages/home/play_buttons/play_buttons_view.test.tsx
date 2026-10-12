import { cleanup, fireEvent, render } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { PlayButtonsView } from "./play_buttons_view";

describe("PlayButtonsView", () => {
  afterEach(cleanup);

  it("shows each queue's count under its button and plays that queue", () => {
    const onPlay = vi.fn();
    const { getByTestId } = render(<PlayButtonsView queues={{ unranked: 6, ranked: 1 }} onPlay={onPlay} />);
    expect(getByTestId("play-unranked-count").textContent).toBe("6 in queue");
    expect(getByTestId("play-ranked-count").textContent).toBe("1 in queue");
    fireEvent.click(getByTestId("play-ranked"));
    fireEvent.click(getByTestId("play-unranked"));
    expect(onPlay.mock.calls).toEqual([["ranked"], ["unranked"]]);
  });

  it("hides the counts while they are unknown", () => {
    const { getByTestId } = render(<PlayButtonsView onPlay={vi.fn()} />);
    expect(getByTestId("play-unranked-count").style.visibility).toBe("hidden");
    expect(getByTestId("play-ranked-count").style.visibility).toBe("hidden");
  });

  it("waits while a Play is handed to the game", () => {
    const onPlay = vi.fn();
    const { getByTestId } = render(
      <PlayButtonsView queues={{ unranked: 0, ranked: 0 }} starting="unranked" onPlay={onPlay} />,
    );
    expect((getByTestId("play-unranked") as HTMLButtonElement).disabled).toBe(true);
    expect((getByTestId("play-ranked") as HTMLButtonElement).disabled).toBe(true);
  });
});
