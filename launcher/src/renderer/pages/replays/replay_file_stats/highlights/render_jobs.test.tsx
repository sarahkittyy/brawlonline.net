import type { RenderJobInfo } from "@dolphin/types";
import { act, fireEvent, render } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { RenderJobs } from "./render_jobs";

vi.mock("@/services", () => ({ useServices: () => ({ dolphinService: { cancelRender: vi.fn() } }) }));

let nextId = 1;
const job = (state: RenderJobInfo["state"], name: string): RenderJobInfo => ({
  jobId: `job-${nextId++}`,
  name,
  output: null,
  paths: ["a.brep"],
  wholeMatch: false,
  clips: 1,
  clip: 1,
  music: true,
  resolution: 720,
  release: null,
  state,
  progress: state === "done" ? 1 : 0,
  error: state === "error" ? "no disk space" : undefined,
});

/** Time going by, `ms` of it. */
const elapse = (ms: number) => {
  act(() => {
    vi.advanceTimersByTime(ms);
  });
};

describe("RenderJobs", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it("lets a Saved row go by itself after 10 s, and an error only once closed", () => {
    const saved = job("done", "saved one");
    const failed = job("error", "failed one");
    const { container, getByLabelText } = render(<RenderJobs jobs={[saved, failed]} />);
    expect(container.textContent).toContain("saved one");
    expect(container.textContent).toContain("failed one");

    elapse(9_000);
    expect(container.textContent).toContain("saved one");
    elapse(1_000);
    expect(container.textContent).not.toContain("saved one");
    elapse(60_000);
    expect(container.textContent).toContain("failed one");

    fireEvent.click(getByLabelText("Close"));
    expect(container.textContent).toBe("");
  });

  it("holds a Saved row while the mouse is over it, then gives it its time again", () => {
    const saved = job("done", "hovered one");
    const { container, getByText } = render(<RenderJobs jobs={[saved]} />);
    const row = getByText("hovered one");
    fireEvent.mouseEnter(row);
    elapse(30_000);
    expect(container.textContent).toContain("hovered one");
    fireEvent.mouseLeave(row);
    elapse(9_000);
    expect(container.textContent).toContain("hovered one");
    elapse(1_000);
    expect(container.textContent).not.toContain("hovered one");
  });

  it("keeps a running row, with no X", () => {
    const running = job("rendering", "running one");
    const { container, queryByLabelText } = render(<RenderJobs jobs={[running]} />);
    elapse(60_000);
    expect(container.textContent).toContain("running one");
    expect(queryByLabelText("Close")).toBeNull();
  });
});
