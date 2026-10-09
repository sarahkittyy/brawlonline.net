import type { QueryKey } from "@tanstack/react-query";
import { useInfiniteQuery } from "@tanstack/react-query";
import { useCallback, useMemo } from "react";

/** A page of a keyset-paginated list: `next` is the cursor of the page after it, null on the last. */
export type CursorPage<T> = { items: T[]; next: string | null };

/** Retries of one page after 429 answers before the list shows an error with a Retry button. */
export const MAX_RATE_LIMIT_RETRIES = 4;
const BASE_BACKOFF_MS = 2000;
const MAX_BACKOFF_MS = 60_000;

/** A 429 from the accounts API (`AccountsError` with status 429 / code `rate_limited`). */
export function isRateLimited(err: unknown): boolean {
  if (err == null || typeof err !== "object") {
    return false;
  }
  const e = err as { status?: unknown; code?: unknown };
  return e.status === 429 || e.code === "rate_limited";
}

/**
 * How long to wait before retry `attempt` (0-based) of a rate-limited page: the server's
 * `Retry-After` when it sent one, else 2 s doubling up to a minute.
 */
export function retryDelayMs(err: unknown, attempt: number): number {
  const retryAfter = (err as { retryAfter?: unknown } | null)?.retryAfter;
  if (typeof retryAfter === "number" && Number.isFinite(retryAfter) && retryAfter >= 0) {
    return Math.min(retryAfter * 1000, MAX_BACKOFF_MS);
  }
  return Math.min(BASE_BACKOFF_MS * 2 ** Math.max(0, attempt), MAX_BACKOFF_MS);
}

export type CursorPages<T> = {
  items: T[];
  /** The first page's extra data (for example the leaderboard's `total`). */
  firstPage: CursorPage<T> | undefined;
  /** The first page is loading. */
  isLoading: boolean;
  /** A page after the first is loading (or waiting to retry). */
  isLoadingMore: boolean;
  hasMore: boolean;
  /** Seconds until the next try of a rate-limited page, while it waits; else null. */
  rateLimitedFor: number | null;
  /** The last page failed for good (a 429 after every retry, or another error). */
  error: Error | null;
  /** Loads the next page, unless one is loading, the list is complete or it failed. */
  loadMore: () => void;
  /** Tries the failed page again. */
  retry: () => void;
};

/**
 * A keyset-paginated list loaded one page at a time: the first page on mount, then the next
 * only when `loadMore` is called (the list calls it when the user scrolls near its bottom).
 * Stops at `next: null`. A 429 is retried after the server's `Retry-After` (or a growing
 * backoff); other errors, and a 429 after {@link MAX_RATE_LIMIT_RETRIES} retries, stop loading
 * until `retry` is called.
 */
export function useCursorPages<T>({
  queryKey,
  fetchPage,
  enabled = true,
}: {
  queryKey: QueryKey;
  fetchPage: (cursor: string | undefined) => Promise<CursorPage<T>>;
  enabled?: boolean;
}): CursorPages<T> {
  const query = useInfiniteQuery({
    queryKey,
    queryFn: ({ pageParam }) => fetchPage(pageParam),
    initialPageParam: undefined as string | undefined,
    getNextPageParam: (last: CursorPage<T>) => last.next ?? undefined,
    enabled,
    retry: (failures, err) => isRateLimited(err) && failures < MAX_RATE_LIMIT_RETRIES,
    retryDelay: (attempt, err) => retryDelayMs(err, attempt),
  });
  const { data, hasNextPage, isFetching, isError, fetchNextPage, refetch, failureCount, failureReason } = query;

  const items = useMemo(() => data?.pages.flatMap((p) => p.items) ?? [], [data]);
  const waiting = isFetching && failureCount > 0 && isRateLimited(failureReason);
  const rateLimitedFor = waiting ? Math.ceil(retryDelayMs(failureReason, failureCount - 1) / 1000) : null;

  const loadMore = useCallback(() => {
    if (hasNextPage && !isFetching && !isError) {
      // Joins a fetch already under way (the rendered state can lag a call behind) instead of
      // cancelling and repeating it.
      void fetchNextPage({ cancelRefetch: false });
    }
  }, [hasNextPage, isFetching, isError, fetchNextPage]);

  const retry = useCallback(() => {
    if (isFetching) {
      return;
    }
    if (data == null) {
      void refetch({ cancelRefetch: false });
    } else {
      void fetchNextPage({ cancelRefetch: false });
    }
  }, [isFetching, data, refetch, fetchNextPage]);

  return {
    items,
    firstPage: data?.pages[0],
    isLoading: query.isPending && enabled,
    isLoadingMore: query.isFetchingNextPage,
    hasMore: Boolean(hasNextPage),
    rateLimitedFor,
    error: isError && !isFetching ? (query.error as Error) : null,
    loadMore,
    retry,
  };
}
