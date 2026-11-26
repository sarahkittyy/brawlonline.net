import net from "net";
import { describe, expect, it } from "vitest";

import { fetchErrorMessage, fetchRetrying, isNoAnswer } from "./fetch_error";

/** A port nothing listens on. */
async function closedPort(): Promise<number> {
  const server = net.createServer();
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address() as net.AddressInfo;
  await new Promise<void>((resolve) => server.close(() => resolve()));
  return port;
}

describe("fetchErrorMessage", () => {
  it("says why Node's fetch got no answer instead of only 'fetch failed'", async () => {
    const port = await closedPort();
    const err = await fetch(`http://127.0.0.1:${port}/`).catch((e: unknown) => e);
    expect(isNoAnswer(err)).toBe(true);
    expect(fetchErrorMessage(err)).toMatch(/^fetch failed: .*ECONNREFUSED/);
  });

  it("names the failed lookup", () => {
    const cause = Object.assign(new Error("getaddrinfo ENOTFOUND api.github.com"), { code: "ENOTFOUND" });
    const err = Object.assign(new TypeError("fetch failed"), { cause });
    expect(fetchErrorMessage(err)).toBe("fetch failed: getaddrinfo ENOTFOUND api.github.com");
  });

  it("keeps a certificate error's code", () => {
    const cause = Object.assign(new Error("unable to get local issuer certificate"), {
      code: "UNABLE_TO_GET_ISSUER_CERT_LOCALLY",
    });
    expect(fetchErrorMessage(Object.assign(new TypeError("fetch failed"), { cause }))).toBe(
      "fetch failed: unable to get local issuer certificate (UNABLE_TO_GET_ISSUER_CERT_LOCALLY)",
    );
  });

  it("lists each address's error when every address failed", () => {
    const cause = Object.assign(
      new AggregateError([
        Object.assign(new Error("connect ETIMEDOUT 140.82.112.6:443"), { code: "ETIMEDOUT" }),
        Object.assign(new Error("connect ENETUNREACH 2606:50c0::1:443"), { code: "ENETUNREACH" }),
      ]),
      { code: "ETIMEDOUT" },
    );
    expect(fetchErrorMessage(Object.assign(new TypeError("fetch failed"), { cause }))).toBe(
      "fetch failed: connect ETIMEDOUT 140.82.112.6:443; connect ENETUNREACH 2606:50c0::1:443",
    );
  });

  it("leaves other errors as they are", () => {
    expect(fetchErrorMessage(new Error("GitHub answered HTTP 403"))).toBe("GitHub answered HTTP 403");
    expect(fetchErrorMessage("x")).toBe("x");
  });
});

describe("fetchRetrying", () => {
  const noAnswer = () => Object.assign(new TypeError("fetch failed"), { cause: new Error("read ECONNRESET") });

  it("asks again while there is no answer", async () => {
    let calls = 0;
    const retries: string[] = [];
    const res = await fetchRetrying(
      async () => {
        calls++;
        if (calls < 3) {
          throw noAnswer();
        }
        return new Response("ok");
      },
      "https://example.invalid/",
      {},
      (err) => retries.push(fetchErrorMessage(err)),
      [1, 1],
    );
    expect(await res.text()).toBe("ok");
    expect(calls).toBe(3);
    expect(retries).toEqual(["fetch failed: read ECONNRESET", "fetch failed: read ECONNRESET"]);
  });

  it("gives up after the last wait", async () => {
    let calls = 0;
    const err = await fetchRetrying(
      async () => {
        calls++;
        throw noAnswer();
      },
      "https://example.invalid/",
      {},
      undefined,
      [1, 1],
    ).catch((e: unknown) => e);
    expect(isNoAnswer(err)).toBe(true);
    expect(calls).toBe(3);
  });

  it("doesn't ask again for other errors or once cancelled", async () => {
    let calls = 0;
    await expect(
      fetchRetrying(
        async () => {
          calls++;
          throw new Error("offline");
        },
        "https://example.invalid/",
        {},
        undefined,
        [1, 1],
      ),
    ).rejects.toThrow("offline");
    expect(calls).toBe(1);

    const abort = new AbortController();
    abort.abort();
    calls = 0;
    await expect(
      fetchRetrying(
        async () => {
          calls++;
          throw noAnswer();
        },
        "https://example.invalid/",
        { signal: abort.signal },
        undefined,
        [1, 1],
      ),
    ).rejects.toThrow("fetch failed");
    expect(calls).toBe(1);
  });
});
