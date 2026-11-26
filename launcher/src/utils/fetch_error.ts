/**
 * Why a fetch() failed. Node's fetch throws a TypeError that says only "fetch failed" when there
 * is no answer at all (no connection, name lookup, timeout, a certificate it doesn't trust) and
 * keeps the reason in `cause`, e.g. "fetch failed: getaddrinfo ENOTFOUND api.github.com".
 */
export function fetchErrorMessage(err: unknown): string {
  if (!(err instanceof Error)) {
    return String(err);
  }
  const reason = causeText((err as { cause?: unknown }).cause);
  return reason && !err.message.includes(reason) ? `${err.message}: ${reason}` : err.message;
}

/** fetch() got no answer at all (as opposed to an HTTP error status or a body that can't be read). */
export function isNoAnswer(err: unknown): boolean {
  return err instanceof TypeError && err.message === "fetch failed";
}

/** How long to wait before asking again after no answer (a dropped connection, a slow name lookup). */
export const NO_ANSWER_RETRY_MS = [1000, 3000];

/** fetch(), asked again after NO_ANSWER_RETRY_MS while there is no answer at all; HTTP errors are answers. */
export async function fetchRetrying<Init extends { signal?: AbortSignal | null }>(
  fetchImpl: (url: string, init?: Init) => Promise<Response>,
  url: string,
  init: Init,
  onRetry?: (err: unknown) => void,
  delaysMs: readonly number[] = NO_ANSWER_RETRY_MS,
): Promise<Response> {
  for (let attempt = 0; ; attempt++) {
    try {
      return await fetchImpl(url, init);
    } catch (err) {
      if (!isNoAnswer(err) || attempt >= delaysMs.length || init.signal?.aborted) {
        throw err;
      }
      onRetry?.(err);
      await new Promise((resolve) => setTimeout(resolve, delaysMs[attempt]));
      if (init.signal?.aborted) {
        throw err;
      }
    }
  }
}

function causeText(cause: unknown, depth = 0): string | null {
  if (cause == null || depth > 3) {
    return null;
  }
  if (typeof cause !== "object") {
    return String(cause);
  }
  const { message, code, errors } = cause as { message?: unknown; code?: unknown; errors?: unknown };
  // Connecting to each of a name's addresses in turn: an AggregateError of each address's error.
  if (Array.isArray(errors) && errors.length > 0) {
    const each = [...new Set(errors.map((e) => causeText(e, depth + 1)).filter((t): t is string => !!t))];
    if (each.length > 0) {
      return each.join("; ");
    }
  }
  const text = typeof message === "string" ? message : "";
  const codeText = typeof code === "string" ? code : "";
  if (text && codeText && !text.includes(codeText)) {
    return `${text} (${codeText})`;
  }
  return text || codeText || causeText((cause as { cause?: unknown }).cause, depth + 1);
}
