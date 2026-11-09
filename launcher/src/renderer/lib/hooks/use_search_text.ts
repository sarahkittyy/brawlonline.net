import { useEffect, useRef, useState } from "react";

/** How long a search waits after the last keystroke. */
export const SEARCH_DEBOUNCE_MS = 300;
/** The least time between two searches sent, however the typing pauses. */
export const SEARCH_MIN_INTERVAL_MS = 1000;

/**
 * The search to run for the text in a search box: trimmed, once typing has paused for
 * `debounceMs`, and never sooner than `minIntervalMs` after the last search it started, so
 * typing with short pauses sends at most one search a second. Clearing the box applies at once
 * and does not count (the whole list is usually still loaded).
 */
export function useSearchText(
  text: string,
  { debounceMs = SEARCH_DEBOUNCE_MS, minIntervalMs = SEARCH_MIN_INTERVAL_MS } = {},
): string {
  const wanted = text.trim();
  const [applied, setApplied] = useState(wanted);
  const lastSearchAt = useRef<number | null>(null);

  useEffect(() => {
    if (wanted === applied) {
      return;
    }
    if (wanted === "") {
      setApplied("");
      return;
    }
    const sinceLast = lastSearchAt.current == null ? Infinity : Date.now() - lastSearchAt.current;
    const wait = Math.max(debounceMs, minIntervalMs - sinceLast);
    const timer = setTimeout(() => {
      lastSearchAt.current = Date.now();
      setApplied(wanted);
    }, wait);
    return () => clearTimeout(timer);
  }, [wanted, applied, debounceMs, minIntervalMs]);

  return applied;
}
