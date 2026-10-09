import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { act, renderHook, waitFor } from "@testing-library/react";
import React from "react";
import { describe, expect, it } from "vitest";

import type { CursorPage } from "./use_cursor_pages";
import { isRateLimited, MAX_RATE_LIMIT_RETRIES, retryDelayMs, useCursorPages } from "./use_cursor_pages";

/** Rows 0..n-1 in pages of `size`; the cursor is the next row's index. Records every call. */
function fakeServer(n: number, size: number) {
  const calls: (string | undefined)[] = [];
  const failures: unknown[] = [];
  const fetchPage = async (cursor: string | undefined): Promise<CursorPage<number>> => {
    calls.push(cursor);
    const fail = failures.shift();
    if (fail) {
      throw fail;
    }
    const start = cursor ? Number(cursor) : 0;
    const end = Math.min(start + size, n);
    const items = Array.from({ length: end - start }, (_, i) => start + i);
    return { items, next: end < n ? String(end) : null };
  };
  return { calls, failures, fetchPage };
}

function wrapper() {
  // The app's defaults (create.tsx): no retries unless the query asks.
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, refetchOnWindowFocus: false } } });
  return ({ children }: { children: React.ReactNode }) => (
    <QueryClientProvider client={client}>{children}</QueryClientProvider>
  );
}

const rateLimited = (retryAfter?: number) =>
  Object.assign(new Error("Too many requests."), { code: "rate_limited", status: 429, retryAfter });

describe("useCursorPages", () => {
  it("loads the first page only, then one page per loadMore, and stops at next: null", async () => {
    const server = fakeServer(5, 2);
    const { result } = renderHook(() => useCursorPages({ queryKey: ["t1"], fetchPage: server.fetchPage }), {
      wrapper: wrapper(),
    });
    await waitFor(() => expect(result.current.items).toEqual([0, 1]));
    expect(server.calls).toEqual([undefined]);
    expect(result.current.hasMore).toBe(true);

    act(() => result.current.loadMore());
    // A second call while the page is loading does nothing.
    act(() => result.current.loadMore());
    await waitFor(() => expect(result.current.items).toEqual([0, 1, 2, 3]));
    expect(server.calls).toEqual([undefined, "2"]);

    act(() => result.current.loadMore());
    await waitFor(() => expect(result.current.items).toEqual([0, 1, 2, 3, 4]));
    expect(result.current.hasMore).toBe(false);
    act(() => result.current.loadMore());
    expect(server.calls).toEqual([undefined, "2", "4"]);
    expect(result.current.firstPage?.next).toBe("2");
  });

  it("waits out a 429 and retries the same page", async () => {
    const server = fakeServer(4, 2);
    const { result } = renderHook(() => useCursorPages({ queryKey: ["t2"], fetchPage: server.fetchPage }), {
      wrapper: wrapper(),
    });
    await waitFor(() => expect(result.current.items).toEqual([0, 1]));
    server.failures.push(rateLimited(0.3));
    act(() => result.current.loadMore());
    await waitFor(() => expect(result.current.rateLimitedFor).toBe(1));
    // While it waits, scrolling does not ask again.
    act(() => result.current.loadMore());
    await waitFor(() => expect(result.current.items).toEqual([0, 1, 2, 3]), { timeout: 3000 });
    expect(server.calls).toEqual([undefined, "2", "2"]);
    expect(result.current.rateLimitedFor).toBeNull();
    expect(result.current.error).toBeNull();
  });

  it("stops after an error until retry", async () => {
    const server = fakeServer(4, 2);
    server.failures.push(new Error("Could not reach the account server."));
    const { result } = renderHook(() => useCursorPages({ queryKey: ["t3"], fetchPage: server.fetchPage }), {
      wrapper: wrapper(),
    });
    await waitFor(() => expect(result.current.error?.message).toBe("Could not reach the account server."));
    // Not a 429: no automatic retry, and loadMore does nothing.
    act(() => result.current.loadMore());
    expect(server.calls).toEqual([undefined]);
    act(() => result.current.retry());
    await waitFor(() => expect(result.current.items).toEqual([0, 1]));
    expect(result.current.error).toBeNull();

    // A failed later page keeps the rows and retries from its own cursor.
    server.failures.push(new Error("boom"));
    act(() => result.current.loadMore());
    await waitFor(() => expect(result.current.error?.message).toBe("boom"));
    expect(result.current.items).toEqual([0, 1]);
    act(() => result.current.retry());
    await waitFor(() => expect(result.current.items).toEqual([0, 1, 2, 3]));
    expect(server.calls).toEqual([undefined, undefined, "2", "2"]);
  });

  it("gives up on a page after the 429 retries", async () => {
    const server = fakeServer(4, 2);
    for (let i = 0; i <= MAX_RATE_LIMIT_RETRIES; i++) {
      server.failures.push(rateLimited(0));
    }
    const { result } = renderHook(() => useCursorPages({ queryKey: ["t4"], fetchPage: server.fetchPage }), {
      wrapper: wrapper(),
    });
    await waitFor(() => expect(result.current.error).not.toBeNull());
    expect(server.calls.length).toBe(MAX_RATE_LIMIT_RETRIES + 1);
    expect(result.current.rateLimitedFor).toBeNull();
  });
});

describe("rate-limit backoff", () => {
  it("follows Retry-After, else doubles from 2 s up to a minute", () => {
    expect(isRateLimited(rateLimited())).toBe(true);
    expect(isRateLimited({ status: 500 })).toBe(false);
    expect(isRateLimited(null)).toBe(false);
    expect(retryDelayMs(rateLimited(17), 0)).toBe(17_000);
    expect(retryDelayMs(rateLimited(3600), 0)).toBe(60_000);
    expect([0, 1, 2, 3, 10].map((a) => retryDelayMs(rateLimited(), a))).toEqual([2000, 4000, 8000, 16000, 60000]);
  });
});
