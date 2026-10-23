import type { FileResult, ReplayService, SearchGamesOptions, SearchGamesResult } from "@replays/types";
import { beforeEach, describe, expect, it, vi } from "vitest";

import type { AuthService } from "@/services/auth/types";

import { ReplayPresenter, useReplays } from "./use_replays";

vi.mock("@/lib/hooks/use_settings", () => {
  const state = { settings: { rootSlpPath: "/replays", extraSlpPaths: [] as string[] } };
  return { useSettings: { getState: () => state } };
});

const file = (name: string) => ({ id: name, fullPath: `/replays/${name}`, fileName: name } as unknown as FileResult);

/** A replay service whose searches each wait until the test answers them. */
const pendingService = () => {
  const searches: { options: SearchGamesOptions; answer: (result: SearchGamesResult) => void }[] = [];
  const service = {
    selectTreeFolder: async () => [],
    searchGames: (options: SearchGamesOptions) =>
      new Promise<SearchGamesResult>((resolve) => searches.push({ options, answer: resolve })),
  } as unknown as ReplayService;
  return { service, searches };
};

const auth = { getCurrentUser: () => null } as unknown as AuthService;
const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

describe("ReplayPresenter", () => {
  beforeEach(() => {
    // The list as an earlier visit left it: one page of /replays/2026-10, more after it.
    useReplays.setState((state) => {
      state.currentFolder = "/replays/2026-10";
      state.files = [file("old.brep")];
      state.loading = false;
      state.loadingMore = false;
      state.hasMoreReplays = true;
      state.continuation = "old-page-2";
    });
  });

  it("finishes a folder load when the list asks for more while it runs (bug report #118)", async () => {
    const { service, searches } = pendingService();
    const presenter = new ReplayPresenter(service, auth);

    // Profile's replays button reloads the replay folder; the list, still the old one, asks for its next page
    // (the highlighted replay isn't in it).
    const load = presenter.loadFolder("/replays", true);
    await flush();
    expect(useReplays.getState().loading).toBe(true);
    void presenter.loadMoreReplays();
    await flush();
    expect(searches.map((s) => s.options.continuation)).toEqual([undefined]);

    searches[0].answer({ files: [file("new.brep")], continuation: "page-2", totalCount: 51 });
    await load;
    const state = useReplays.getState();
    expect(state.loading).toBe(false);
    expect(state.files.map((f) => f.fileName)).toEqual(["new.brep"]);
    expect(state.continuation).toBe("page-2");
  });

  it("drops a page a newer folder load has replaced", async () => {
    const { service, searches } = pendingService();
    const presenter = new ReplayPresenter(service, auth);

    const more = presenter.loadMoreReplays();
    const load = presenter.loadFolder("/replays", true);
    await flush();
    expect(searches.map((s) => s.options.continuation)).toEqual(["old-page-2", undefined]);

    searches[1].answer({ files: [file("new.brep")], continuation: undefined, totalCount: 1 });
    await load;
    searches[0].answer({ files: [file("older.brep")], continuation: "old-page-3", totalCount: 100 });
    await more;
    const state = useReplays.getState();
    expect(state.loading).toBe(false);
    expect(state.loadingMore).toBe(false);
    expect(state.files.map((f) => f.fileName)).toEqual(["new.brep"]);
    expect(state.hasMoreReplays).toBe(false);
  });

  it("finishes a load still running when the same folder is opened again", async () => {
    const { service, searches } = pendingService();
    const presenter = new ReplayPresenter(service, auth);

    const first = presenter.loadFolder("/replays", true);
    await flush();
    // The folder tree's click on the folder being loaded (no forced reload).
    const second = presenter.loadFolder("/replays");
    await flush();
    expect(searches).toHaveLength(2);

    searches[0].answer({ files: [file("a.brep")], continuation: undefined, totalCount: 1 });
    searches[1].answer({ files: [file("a.brep")], continuation: undefined, totalCount: 1 });
    await Promise.all([first, second]);
    expect(useReplays.getState().loading).toBe(false);
  });
});
