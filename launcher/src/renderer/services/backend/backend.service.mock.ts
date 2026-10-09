import { Preconditions } from "@common/preconditions";
import type { PlayKey } from "@dolphin/types";

import type { AuthService } from "../auth/types";
import { delayAndMaybeError } from "../utils";
import type {
  BackendService,
  LeaderboardEntry,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryItem,
  MatchHistoryPage,
  RankedProfile,
  UserData,
} from "./types";

const SHOULD_ERROR = false;

const fakeUserId = "userid";

type SavedUserData = UserData & { savedMessages: string[] };

const savedMessages = [
  "ggs",
  "one more",
  "brb",
  "good luck",

  "well played",
  "that was fun",
  "thanks",
  "too good",

  "sorry",
  "my b",
  "lol",
  "wow",

  "gotta go",
  "one sec",
  "let's play again later",
  "bad connection",
];

const mockRankedProfile: RankedProfile = {
  rating: 1523.4,
  setsPlayed: 42,
  position: 23,
  rankedPlayers: 137,
};

const MOCK_PAGE_SIZE = 50;

// 137 players; the mock user (`DEMO#0`) is 23rd, as in `mockRankedProfile`.
const mockLeaderboard: LeaderboardEntry[] = Array.from({ length: 137 }, (_, i) => {
  const setsPlayed = 5 + ((i * 37) % 120);
  const wins = Math.round(setsPlayed * (0.75 - i / 400));
  return {
    position: i + 1,
    uid: i === 22 ? fakeUserId : `mock-player-${i + 1}`,
    displayName: i === 22 ? "Demo user 0" : `Player ${i + 1}`,
    connectCode: i === 22 ? "DEMO#0" : `PLYR#${100 + i}`,
    rating: i === 22 ? mockRankedProfile.rating : 2150 - i * 9.3 - (i % 3) * 0.4,
    setsPlayed,
    wins,
    losses: setsPlayed - wins,
  };
});

function mockMatchHistory(): MatchHistoryItem[] {
  const opponents = mockLeaderboard.filter((e) => e.uid !== fakeUserId);
  const modes: MatchHistoryItem["mode"][] = ["ranked", "unranked", "ranked", "direct", "ranked"];
  let rating = mockRankedProfile.rating;
  const start = Date.UTC(2026, 9, 8, 20, 0, 0);
  return Array.from({ length: 45 }, (_, i) => {
    const opp = opponents[(i * 7) % opponents.length];
    const mode = modes[i % modes.length];
    const won = i % 3 !== 1;
    const me = { uid: fakeUserId, displayName: "Demo user 0", connectCode: "DEMO#0" };
    const them = { uid: opp.uid, displayName: opp.displayName, connectCode: opp.connectCode };
    const createdAt = new Date(start - i * 47 * 60 * 1000).toISOString();
    if (mode !== "ranked") {
      const games = 2 + (i % 5);
      const myWins = won ? Math.ceil(games / 2) : Math.floor(games / 2);
      const none = { ratingBefore: null, ratingAfter: null, ratingChange: null };
      return {
        matchId: `mode.${mode}-mock-${i}`,
        mode,
        createdAt,
        status: "ASSIGNED",
        ranked: false,
        players: [
          { ...me, wins: myWins, ...none },
          { ...them, wins: games - myWins, ...none },
        ],
        winner: null,
        endReason: null,
      };
    }
    const abandoned = i === 4;
    const change = won ? 14.2 + (i % 4) : -(12.8 + (i % 5));
    const before = rating - change;
    rating = before;
    const oppBefore = opp.rating + change;
    return {
      matchId: `mode.ranked-mock-${i}`,
      mode,
      createdAt,
      status: abandoned ? "ABANDONED" : "COMPLETE",
      ranked: true,
      players: [
        {
          ...me,
          wins: abandoned ? 1 : won ? 2 : i % 2,
          ratingBefore: before,
          ratingAfter: before + change,
          ratingChange: change,
        },
        {
          ...them,
          wins: abandoned ? 0 : won ? i % 2 : 2,
          ratingBefore: oppBefore,
          ratingAfter: oppBefore - change,
          ratingChange: -change,
        },
      ],
      winner: won ? fakeUserId : opp.uid,
      endReason: abandoned ? `abandoned by ${opp.uid}` : null,
    };
  });
}

/** Cursor = index of the next row, as a string (the real cursors are opaque). */
function mockPage<T>(rows: T[], cursor: string | undefined, size: number): { rows: T[]; next: string | null } {
  const start = cursor ? Number(cursor) : 0;
  const end = start + size;
  return { rows: rows.slice(start, end), next: end < rows.length ? String(end) : null };
}

class MockBackendClient implements BackendService {
  private fakeUsers: Map<string, SavedUserData> = new Map();

  constructor(private authService: AuthService) {
    this.addFakeUser(fakeUserId);
  }

  private addFakeUser(userId: string, displayName?: string): void {
    const numUsers = this.fakeUsers.size;

    this.fakeUsers.set(userId, {
      playKey: {
        uid: userId,
        connectCode: `DEMO#${numUsers}`,
        playKey: "playkey",
        displayName: displayName ?? `Demo user ${numUsers}`,
      },
      savedMessages,
      rankedNetplayProfile: mockRankedProfile,
    });
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async validateUserId(userId: string): Promise<{ displayName: string; connectCode: string }> {
    const userData = this.fakeUsers.get(userId);
    if (!userData || !userData.playKey) {
      throw new Error(`No user with ID: ${userId}`);
    }

    return {
      displayName: userData.playKey.displayName,
      connectCode: userData.playKey.connectCode,
    };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchUserData(): Promise<UserData | undefined> {
    const user = this.authService.getCurrentUser();
    Preconditions.checkExists(user, "No user logged in");

    if (!this.fakeUsers.has(user.uid)) {
      this.addFakeUser(user.uid, user.displayName);
    }
    const userData = this.fakeUsers.get(user.uid);
    if (!userData) {
      return undefined;
    }

    return userData;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchRankedNetplayProfile(_userId: string): Promise<RankedProfile | undefined> {
    return Promise.resolve(mockRankedProfile);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchLeaderboard(after?: string): Promise<LeaderboardPage> {
    const { rows, next } = mockPage(mockLeaderboard, after, MOCK_PAGE_SIZE);
    return { entries: rows, next, total: mockLeaderboard.length };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchMatchHistory(mode: MatchHistoryFilter, before?: string): Promise<MatchHistoryPage> {
    const all = mockMatchHistory().filter((m) => mode === "all" || (mode === "ranked") === m.ranked);
    const { rows, next } = mockPage(all, before, 20);
    return { matches: rows, next };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async assertPlayKey(_playKey: PlayKey) {
    // Do nothing
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async deletePlayKey(): Promise<void> {
    // Do nothing
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async changeDisplayName(name: string) {
    const user = this.authService.getCurrentUser();
    Preconditions.checkExists(user, "No user logged in");

    const userData = this.fakeUsers.get(user.uid);
    Preconditions.checkExists(userData, `No user with id: ${user.uid}`);

    userData.playKey!.displayName = name;
    this.fakeUsers.set(user.uid, userData);
    await this.authService.updateDisplayName(name);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async initializeNetplay(_codeStart: string): Promise<void> {
    // Do nothing
  }
}

export default function createMockBackendClient(authService: AuthService): BackendService {
  return new MockBackendClient(authService);
}
