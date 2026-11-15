import type React from "react";
import { useCallback, useEffect, useLayoutEffect, useMemo, useReducer, useRef, useState } from "react";

/** A row's height until it has been measured (px). */
const ESTIMATE = 64;
/** Rows kept laid out above and below what shows (px): scrolling a little doesn't re-render. */
const OVERSCAN = 500;
/** Rows laid out before the scroller is measured (a test's, which has no layout). */
const FIRST = 40;

const scrollerOf = (el: HTMLElement | null): HTMLElement | null => {
  for (let p = el?.parentElement ?? null; p; p = p.parentElement) {
    const { overflowY } = getComputedStyle(p);
    if (overflowY === "auto" || overflowY === "scroll") {
      return p;
    }
  }
  return null;
};

/**
 * Only the rows near what shows are in the page (the Highlights tab lists every replay's, 1600+ for a player with a
 * month of replays: all of them laid out made each frame of a scroll take ~35 ms, Sarah's board). The others are
 * space of their measured height (an estimate until they have been seen). `keys` are the rows' (each row's element
 * gets `measure(key)` as its ref); `listRef` is the element they are in, inside a scrolling element.
 */
export const useWindowedRows = (keys: readonly string[], listRef: React.RefObject<HTMLElement>) => {
  const heights = useRef(new Map<string, number>());
  const [version, remeasured] = useReducer((x: number) => x + 1, 0);
  const [range, setRange] = useState({ start: 0, end: FIRST });

  // Where each row starts in the list (px), and the end.
  const offsets = useMemo(() => {
    let sum = 0;
    let n = 0;
    for (const h of heights.current.values()) {
      sum += h;
      n++;
    }
    const guess = n > 0 ? sum / n : ESTIMATE;
    const out = new Float64Array(keys.length + 1);
    for (let i = 0; i < keys.length; i++) {
      out[i + 1] = out[i] + (heights.current.get(keys[i]) ?? guess);
    }
    return out;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [keys, version]);

  // The first row ending after `y`.
  const rowAt = useCallback(
    (y: number) => {
      let lo = 0;
      let hi = keys.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (offsets[mid + 1] <= y) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      return lo;
    },
    [offsets, keys.length],
  );

  const update = useCallback(() => {
    const list = listRef.current;
    const scroller = scrollerOf(list);
    if (!list || !scroller || scroller.clientHeight === 0) {
      return;
    }
    // What shows, in the list's own coordinates.
    const top = scroller.getBoundingClientRect().top - list.getBoundingClientRect().top;
    const bottom = top + scroller.clientHeight;
    setRange((r) => {
      const need = { start: rowAt(top - OVERSCAN / 2), end: Math.min(keys.length, rowAt(bottom + OVERSCAN / 2) + 1) };
      if (need.start >= r.start && need.end <= r.end && r.end <= keys.length) {
        return r;
      }
      return { start: rowAt(top - OVERSCAN), end: Math.min(keys.length, rowAt(bottom + OVERSCAN) + 1) };
    });
  }, [listRef, rowAt, keys.length]);

  useLayoutEffect(update, [update]);

  useEffect(() => {
    const scroller = scrollerOf(listRef.current);
    if (!scroller) {
      return;
    }
    let frame = 0;
    const onScroll = () => {
      if (!frame) {
        frame = requestAnimationFrame(() => {
          frame = 0;
          update();
        });
      }
    };
    scroller.addEventListener("scroll", onScroll, { passive: true });
    const resized = typeof ResizeObserver === "undefined" ? null : new ResizeObserver(onScroll);
    resized?.observe(scroller);
    return () => {
      scroller.removeEventListener("scroll", onScroll);
      resized?.disconnect();
      cancelAnimationFrame(frame);
    };
  }, [listRef, update]);

  // Each row's height as laid out (it changes with the width: a row's players wrap).
  const observer = useMemo(() => {
    if (typeof ResizeObserver === "undefined") {
      return null;
    }
    return new ResizeObserver((items) => {
      let changed = false;
      for (const item of items) {
        const el = item.target as HTMLElement;
        const key = el.dataset.windowKey;
        const h = el.offsetHeight;
        if (key != null && h > 0 && heights.current.get(key) !== h) {
          heights.current.set(key, h);
          changed = true;
        }
      }
      if (changed) {
        remeasured();
      }
    });
  }, []);
  useEffect(() => () => observer?.disconnect(), [observer]);

  const refs = useRef(new Map<string, (el: HTMLElement | null) => void>());
  const measure = useCallback(
    (key: string) => {
      let ref = refs.current.get(key);
      if (!ref) {
        let seen: HTMLElement | null = null;
        ref = (el: HTMLElement | null) => {
          if (seen && observer) {
            observer.unobserve(seen);
          }
          seen = el;
          if (el) {
            el.dataset.windowKey = key;
            observer?.observe(el);
          }
        };
        refs.current.set(key, ref);
      }
      return ref;
    },
    [observer],
  );

  /** Scrolls row `index` into view, below `coverTop` px and above `coverBottom` px of the scroller (sticky bars). */
  const scrollToRow = useCallback(
    (index: number, coverTop = 0, coverBottom = 0) => {
      const list = listRef.current;
      const scroller = scrollerOf(list);
      if (!list || !scroller || index < 0 || index >= keys.length) {
        return;
      }
      const at = list.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
      const top = at + offsets[index];
      const bottom = at + offsets[index + 1];
      if (top < coverTop) {
        scroller.scrollTop += top - coverTop - 8;
      } else if (bottom > scroller.clientHeight - coverBottom) {
        scroller.scrollTop += bottom - (scroller.clientHeight - coverBottom) + 8;
      }
    },
    [listRef, keys.length, offsets],
  );

  const start = Math.min(range.start, keys.length);
  const end = Math.min(range.end, keys.length);
  return {
    start,
    end,
    /** Space for the rows before `start` and after `end`. */
    before: offsets[start],
    after: offsets[keys.length] - offsets[end],
    measure,
    scrollToRow,
  };
};
