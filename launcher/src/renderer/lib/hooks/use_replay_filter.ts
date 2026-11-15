import type { ReplayFilter } from "@database/filters/types";
import type { ReplayPlayer } from "@replays/types";
import { create } from "zustand";
import { combine } from "zustand/middleware";

import { convertToReplayFilters, ME_MARKER, parseQuery } from "../query_language/parser";
import { ReplaySortOption, SortDirection } from "../replay_file_sort";

/** The search the replays are filtered by: a `code:` term for each player tagged, then what is typed. */
const composeSearch = (players: readonly ReplayPlayer[], text: string) =>
  [...players.map((p) => `code:${p.connectCode}`), text.trim()].filter(Boolean).join(" ");

export const useReplayFilter = create(
  combine(
    {
      /** The query the list is filtered by (composeSearch). */
      searchText: "",
      /** What is typed in the search box (the query language's, README.md in query_language). */
      text: "",
      /** The players tagged in the search box: the games every one of them played in. */
      players: [] as ReplayPlayer[],
      /** Counts the times the filter was cleared (the search box empties its own text). */
      resets: 0,
      sortBy: ReplaySortOption.DATE,
      sortDirection: SortDirection.DESC,
      hideShortGames: true,
    },
    (set) => ({
      setSearchText: (text: string) => set((s) => ({ text, searchText: composeSearch(s.players, text) })),
      setPlayers: (players: ReplayPlayer[]) => set((s) => ({ players, searchText: composeSearch(players, s.text) })),
      setSortBy: (sortBy: ReplaySortOption) => set({ sortBy }),
      setSortDirection: (sortDirection: SortDirection) => set({ sortDirection }),
      setHideShortGames: (hideShortGames: boolean) => set({ hideShortGames }),
      resetFilter: () => {
        set((s) => ({
          searchText: "",
          text: "",
          players: [],
          resets: s.resets + 1,
          hideShortGames: false,
        }));
      },
    }),
  ),
);

/**
 * Converts the current filter state to ReplayFilter array
 *
 * This function now uses the query language parser to convert search text into structured filters.
 * The hideShortGames checkbox is still supported as a quick filter and merged with query filters.
 */
export const buildReplayFilters = (
  hideShortGames: boolean,
  searchText: string,
  userId: string | undefined,
): ReplayFilter[] => {
  console.log("Building filters for user:", userId);
  const filters: ReplayFilter[] = [];

  // Parse the search text using the query language parser
  if (searchText && searchText.trim() !== "") {
    const parsed = parseQuery(searchText);
    let queryFilters = convertToReplayFilters(parsed.filters);

    // Resolve @me to actual user ID, removing filters where resolution fails (not logged in)
    queryFilters = queryFilters.filter((filter) => {
      if (filter.type !== "player") {
        return true;
      }

      if (filter.userId === ME_MARKER) {
        // We want to replace the ME_MARKER with our actual user id
        if (!userId) {
          // User not logged in, remove this filter since it's invalid
          return false;
        }

        filter.userId = userId;
        return true;
      }

      return true;
    });

    // Add all query filters
    filters.push(...queryFilters);

    // Log any parsing errors to console (for debugging)
    if (parsed.errors.length > 0) {
      console.warn("Query parsing errors:", parsed.errors);
    }
  }

  // Apply hideShortGames filter (merge with any duration filter from query)
  if (hideShortGames) {
    // Check if there's already a duration filter from the query
    const existingDurationFilter = filters.find((f) => f.type === "duration");

    if (!existingDurationFilter) {
      // Filter out the short games nobody won (quits, disconnects, handwarmers): a short game
      // that ended in a game set is a real game and stays
      filters.push({
        type: "duration",
        minFrames: 30 * 60 - 123, // 30 seconds minus the offset
        orWon: true,
      });
    }
  }

  return filters;
};
