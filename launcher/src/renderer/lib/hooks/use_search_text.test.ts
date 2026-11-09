import { act, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { SEARCH_DEBOUNCE_MS, SEARCH_MIN_INTERVAL_MS, useSearchText } from "./use_search_text";

describe("useSearchText", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const typing = () => renderHook(({ text }) => useSearchText(text), { initialProps: { text: "" } });

  it("applies the trimmed text once typing pauses", () => {
    const { result, rerender } = typing();
    rerender({ text: "b" });
    act(() => {
      vi.advanceTimersByTime(200);
    });
    rerender({ text: "be " });
    act(() => {
      vi.advanceTimersByTime(SEARCH_DEBOUNCE_MS - 1);
    });
    expect(result.current).toBe("");
    act(() => {
      vi.advanceTimersByTime(1);
    });
    expect(result.current).toBe("be");
    // Spaces around it change nothing.
    rerender({ text: "  be" });
    act(() => {
      vi.advanceTimersByTime(5000);
    });
    expect(result.current).toBe("be");
  });

  it("starts at most one search a second however the typing pauses", () => {
    const { result, rerender } = typing();
    const applied: { at: number; text: string }[] = [];
    let last = "";
    const start = Date.now();
    // A key every 350 ms (each pause just longer than the debounce) for 5 s.
    for (let i = 1; i <= 15; i++) {
      rerender({ text: "abcdefghijklmnop".slice(0, i) });
      for (let t = 0; t < 350; t += 50) {
        act(() => {
          vi.advanceTimersByTime(50);
        });
        if (result.current !== last) {
          last = result.current;
          applied.push({ at: Date.now() - start, text: last });
        }
      }
    }
    act(() => {
      vi.advanceTimersByTime(SEARCH_MIN_INTERVAL_MS);
    });
    expect(result.current).toBe("abcdefghijklmno");
    for (let i = 1; i < applied.length; i++) {
      expect(applied[i].at - applied[i - 1].at).toBeGreaterThanOrEqual(SEARCH_MIN_INTERVAL_MS);
    }
    // Without the spacing it would have been 15.
    expect(applied.length).toBeLessThanOrEqual(6);
  });

  it("clears at once", () => {
    const { result, rerender } = typing();
    rerender({ text: "zz" });
    act(() => {
      vi.advanceTimersByTime(SEARCH_DEBOUNCE_MS);
    });
    expect(result.current).toBe("zz");
    rerender({ text: " " });
    expect(result.current).toBe("");
  });
});
