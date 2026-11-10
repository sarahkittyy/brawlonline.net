import { act, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { TOURNAMENTS_URL_REFRESH_MS, useTournamentsUrl, webUrl } from "./use_tournaments_url";

const fetchTournamentsUrl = vi.fn<() => Promise<string | null>>();
// One object, as the services context gives.
const services = { backendService: { fetchTournamentsUrl } };
vi.mock("@/services", () => ({ useServices: () => services }));
vi.mock("electron-log", () => ({ default: { warn: vi.fn() } }));

describe("webUrl", () => {
  it("lets only web pages through", () => {
    expect(webUrl("https://start.gg/catfight")).toBe("https://start.gg/catfight");
    expect(webUrl("http://example.com/x")).toBe("http://example.com/x");
    expect(webUrl(null)).toBeNull();
    expect(webUrl("")).toBeNull();
    expect(webUrl("start.gg/catfight")).toBeNull();
    expect(webUrl("file:///C:/Windows/notepad.exe")).toBeNull();
    expect(webUrl("javascript:alert(1)")).toBeNull();
  });
});

describe("useTournamentsUrl", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    fetchTournamentsUrl.mockReset();
  });
  afterEach(() => vi.useRealTimers());

  const settle = () => act(async () => await vi.advanceTimersByTimeAsync(0));

  it("follows the server: none at first, the link once named, none again once cleared", async () => {
    fetchTournamentsUrl.mockResolvedValue("https://start.gg/catfight");
    const { result } = renderHook(() => useTournamentsUrl());
    expect(result.current).toBeNull();
    await settle();
    expect(result.current).toBe("https://start.gg/catfight");

    fetchTournamentsUrl.mockResolvedValue(null);
    await act(async () => await vi.advanceTimersByTimeAsync(TOURNAMENTS_URL_REFRESH_MS));
    expect(result.current).toBeNull();
    expect(fetchTournamentsUrl).toHaveBeenCalledTimes(2);
  });

  it("keeps the last answer when the server cannot be reached", async () => {
    fetchTournamentsUrl.mockResolvedValue("https://start.gg/catfight");
    const { result } = renderHook(() => useTournamentsUrl());
    await settle();
    fetchTournamentsUrl.mockRejectedValue(new Error("offline"));
    await act(async () => await vi.advanceTimersByTimeAsync(TOURNAMENTS_URL_REFRESH_MS));
    expect(result.current).toBe("https://start.gg/catfight");
  });

  it("shows nothing for a link that is not a web page", async () => {
    fetchTournamentsUrl.mockResolvedValue("file:///C:/Windows/notepad.exe");
    const { result } = renderHook(() => useTournamentsUrl());
    await settle();
    expect(result.current).toBeNull();
  });
});
