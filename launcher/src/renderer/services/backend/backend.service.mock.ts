import { Preconditions } from "@common/preconditions";
import { defaultRuleset, normalizeRuleset, parseRulesetCode, rulesetProblem } from "@common/rulesets";
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
  MyRulesets,
  PublishRulesetRequest,
  Rank,
  RankedProfile,
  RankTier,
  RecentRuleset,
  RoomKind,
  RoomList,
  RulesetDraft,
  RulesetDraftRequest,
  RulesetPage,
  RulesetView,
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

// The server's cutoffs (common::ranked::RANK_FLOORS): each division's lowest rating, Bronze 3 first.
// The server holds the real rank (with its demotion buffer); the mock derives it from the rating.
const MOCK_RANK_FLOORS = [
  -Infinity,
  1050,
  1100,
  1150,
  1200,
  1250,
  1300,
  1350,
  1400,
  1450,
  1510,
  1570,
  1625,
  1750,
  1875,
  2000,
  2085,
  2170,
  2250,
];
const MOCK_TIERS: RankTier[] = ["bronze", "silver", "gold", "emerald", "diamond", "master", "legendary"];

function mockRank(rating: number): Rank {
  const index = MOCK_RANK_FLOORS.filter((floor) => rating >= floor).length - 1;
  const tier = MOCK_TIERS[Math.floor(index / 3)];
  return tier === "legendary" ? { tier, division: null } : { tier, division: (3 - (index % 3)) as 1 | 2 | 3 };
}

/** `?mockRank=gold-2`, `?mockRank=legendary` or `?mockRank=placing-3` in the dev URL sets the mock user's rank. */
function mockRankOverride(): Pick<RankedProfile, "rank" | "setsToPlace"> | undefined {
  const value = typeof window === "undefined" ? null : new URLSearchParams(window.location.search).get("mockRank");
  const m = value?.match(/^(bronze|silver|gold|emerald|diamond|master|legendary|placing)(?:-(\d+))?$/);
  if (!m) {
    return undefined;
  }
  if (m[1] === "placing") {
    return { rank: null, setsToPlace: Number(m[2] ?? 3) };
  }
  const division = m[2] ? (Number(m[2]) as 1 | 2 | 3) : null;
  return { rank: { tier: m[1] as RankTier, division }, setsToPlace: 0 };
}

const mockRankOverrides = mockRankOverride();
/** `?mockRank=placing-N`: the mock user is placing, as the server shows it (off the leaderboard, rating hidden). */
const mockPlacing = (mockRankOverrides?.setsToPlace ?? 0) > 0;

const mockRankedProfile: RankedProfile = {
  rating: 1523.4,
  setsPlayed: 42,
  position: 23,
  rankedPlayers: 137,
  rank: mockRank(1523.4),
  setsToPlace: 0,
  ...mockRankOverrides,
  ...(mockPlacing && {
    setsPlayed: 5 - (mockRankOverrides?.setsToPlace ?? 0),
    position: null,
    rankedPlayers: 136,
  }),
};

const MOCK_PAGE_SIZE = 50;

/** The room list the mock backend serves (the same rooms as the Storybook stories). */
export function mockRoomList(): RoomList {
  return {
    online: 128,
    queues: { unranked: 6, ranked: 3 },
    playing: { unranked: 14, ranked: 8 },
    updatedAt: new Date().toISOString(),
    rooms: [
      {
        code: "KFQB",
        host: "Sarah",
        players: 1,
        openSlots: 2,
        mode: "1v1",
        status: "waiting",
        names: ["Sarah"],
        joinable: true,
      },
      {
        code: "TRZM",
        host: "Wario Fan",
        players: 3,
        openSlots: 4,
        mode: "ffa",
        status: "in-game",
        names: ["Wario Fan", "Ness", "Lucas"],
        joinable: true,
      },
      {
        code: "HBPL",
        host: "Mango",
        players: 4,
        openSlots: 4,
        mode: "teams",
        status: "in-game",
        names: ["Mango", "Zain", "Cody", "Hbox"],
        joinable: false,
      },
    ],
  };
}

// 137 placed players; the mock user (`DEMO#0`) is 23rd, as in `mockRankedProfile`, or not listed while placing.
const mockLeaderboard: LeaderboardEntry[] = Array.from({ length: 137 }, (_, i): LeaderboardEntry => {
  const setsPlayed = 5 + ((i * 37) % 120);
  const wins = Math.round(setsPlayed * (0.75 - i / 400));
  return {
    position: i + 1,
    uid: i === 22 ? fakeUserId : `mock-player-${i + 1}`,
    displayName: i === 22 ? "Demo user 0" : `Player ${i + 1}`,
    connectCode: i === 22 ? "DEMO#0" : `PLYR#${100 + i}`,
    rating: i === 22 ? mockRankedProfile.rating : 2300 - i * 10.5 - (i % 3) * 0.4,
    setsPlayed,
    wins,
    losses: setsPlayed - wins,
    rank: i === 22 ? mockRankedProfile.rank : mockRank(2300 - i * 10.5),
    setsToPlace: 0,
    // Moved since yesterday: mostly a place or two, some still, a few big climbs, a few new.
    positionChange: i % 11 === 6 ? null : [0, 2, -1, 0, 5, -3, 1, 0, -1, 11, 1][i % 11],
  };
})
  .filter((e) => !(mockPlacing && e.uid === fakeUserId))
  .map((e, i) => ({ ...e, position: i + 1 }));

function mockMatchHistory(): MatchHistoryItem[] {
  const opponents = mockLeaderboard.filter((e) => e.uid !== fakeUserId);
  const modes: MatchHistoryItem["mode"][] = ["ranked", "unranked", "room", "ranked", "direct", "room", "ranked"];
  let rating = mockRankedProfile.rating;
  const start = Date.UTC(2026, 9, 8, 20, 0, 0);
  return Array.from({ length: 45 }, (_, i) => {
    const opp = opponents[(i * 7) % opponents.length];
    const mode = modes[i % modes.length];
    const won = i % 3 !== 1;
    const me = { uid: fakeUserId, displayName: "Demo user 0", connectCode: "DEMO#0" };
    const them = { uid: opp.uid, displayName: opp.displayName, connectCode: opp.connectCode };
    const createdAt = new Date(start - i * 47 * 60 * 1000).toISOString();
    if (mode === "room") {
      const more = opponents
        .filter((o) => o.uid !== opp.uid)
        .slice(i % 5, (i % 5) + 2)
        .map((o) => ({ uid: o.uid, displayName: o.displayName, connectCode: o.connectCode }));
      return mockRoomGame(i, createdAt, [me, them, ...more]);
    }
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
          // While placing the server hides the player's own numbers.
          ...(mockPlacing
            ? { ratingBefore: null, ratingAfter: null, ratingChange: null, ratingHidden: true }
            : { ratingBefore: before, ratingAfter: before + change, ratingChange: change }),
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

/** A room's game: a free-for-all of four, a 2v2 or a 1v1 on slots 1 and 3, in turn. */
function mockRoomGame(
  i: number,
  createdAt: string,
  people: { uid: string; displayName: string; connectCode: string }[],
): MatchHistoryItem {
  const none = { ratingBefore: null, ratingAfter: null, ratingChange: null };
  const kind = (["ffa", "teams", "1v1"] as const)[i % 3];
  const base = { matchId: `mode.room-KFQB-mock-${i}`, mode: "room" as const, createdAt, status: "ASSIGNED" };
  const rest = { ranked: false, winner: null, endReason: null, room: { code: "KFQB", mode: kind } };
  if (kind === "1v1") {
    const won = i % 2 === 0;
    return {
      ...base,
      ...rest,
      players: [
        { ...people[0], wins: won ? 1 : 0, port: 1, team: null, place: won ? 1 : 2, ...none },
        { ...people[1], wins: won ? 0 : 1, port: 3, team: null, place: won ? 2 : 1, ...none },
      ],
    };
  }
  const ffaPlaces = i % 2 === 0 ? [2, 1, 4, 3] : [1, 3, 2, 4];
  const teamPlaces = i % 2 === 0 ? [1, 2, 1, 2] : [2, 1, 2, 1];
  const places = kind === "ffa" ? ffaPlaces : teamPlaces;
  return {
    ...base,
    ...rest,
    players: people.slice(0, 4).map((p, k) => ({
      ...p,
      wins: places[k] === 1 ? 1 : 0,
      port: k + 1,
      team: kind === "teams" ? k % 2 : null,
      // The last player left one free-for-all.
      place: kind === "ffa" && k === 3 && i % 2 === 0 ? null : places[k],
      ...none,
    })),
  };
}

/** Cursor = index of the next row, as a string (the real cursors are opaque). */
function mockPage<T>(rows: T[], cursor: string | undefined, size: number): { rows: T[]; next: string | null } {
  const start = cursor ? Number(cursor) : 0;
  const end = start + size;
  return { rows: rows.slice(start, end), next: end < rows.length ? String(end) : null };
}

/** A published ruleset of someone else's, so a code works in the mock (`KFQBTR`). */
function mockWeekly(): RulesetView {
  return {
    code: "KFQBTR",
    ruleset: {
      ...defaultRuleset(),
      name: "NorCal Weekly",
      version: 3,
      by: "sarah",
      bestOf: 3,
      strikes: [1, 2, 1],
      bans: 2,
      dsr: true,
    },
    hash: "0".repeat(64),
    createdAt: "2026-10-11T12:00:00.000Z",
    newer: null,
    link: "https://brawlonline.net/r/KFQBTR",
    games: 1423,
  };
}

class MockBackendClient implements BackendService {
  private fakeUsers: Map<string, SavedUserData> = new Map();
  private rulesets: RulesetView[] = [mockWeekly()];
  private drafts: RulesetDraft[] = [];
  private recent: string[] = [];
  private remembered: string | null = null;

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
    const all = mockMatchHistory().filter(
      (m) =>
        mode === "all" ||
        (mode === "ranked" && m.ranked) ||
        (mode === "unranked" && (m.mode === "unranked" || m.mode === "direct")) ||
        (mode === "rooms" && m.mode === "room"),
    );
    const { rows, next } = mockPage(all, before, 20);
    return { matches: rows, next };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchRooms(): Promise<RoomList> {
    return mockRoomList();
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async setRoomName(_kind: RoomKind, name: string | null): Promise<string | null> {
    return name?.trim() || null;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchRuleset(code: string): Promise<RulesetView> {
    const found = this.rulesets.find((r) => r.code === parseRulesetCode(code));
    if (!found) {
      throw new Error("No ruleset has this code");
    }
    return this._withNewer(found);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchPublicRulesets(q: string, after?: string): Promise<RulesetPage> {
    const text = q.trim().toLowerCase();
    // Each ruleset's newest visible version (same name and publisher in the mock), most played first.
    const latest = new Map<string, RulesetView>();
    for (const r of this.rulesets.filter((x) => !x.hidden)) {
      const key = `${r.ruleset.by}/${r.ruleset.name}`;
      const have = latest.get(key);
      if (!have || have.ruleset.version < r.ruleset.version) {
        latest.set(key, r);
      }
    }
    const all = [...latest.values()]
      .filter(
        (r) =>
          !text ||
          r.ruleset.name.toLowerCase().includes(text) ||
          r.ruleset.by.toLowerCase().includes(text) ||
          r.code.toLowerCase().includes(text),
      )
      .sort((a, b) => b.games - a.games || b.createdAt.localeCompare(a.createdAt))
      .map(({ hidden: _hidden, ...r }) => ({ ...r, newer: null }));
    const start = after ? all.findIndex((r) => r.code === after) + 1 : 0;
    const rows = all.slice(start, start + 30);
    return { rulesets: rows, next: start + 30 < all.length ? rows[rows.length - 1].code : null };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchMyRulesets(): Promise<MyRulesets> {
    const published = this.rulesets.filter((r) => r.ruleset.by === "Mock User").map((r) => this._withNewer(r));
    return { drafts: [...this.drafts], published: published.reverse() };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchRecentRulesets(): Promise<RecentRuleset[]> {
    return this.recent.map((code) => ({ ...this._withNewer(this.rulesets.find((r) => r.code === code)!), usedAt: "" }));
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async publishRuleset(req: PublishRulesetRequest): Promise<RulesetView> {
    const draft = "draftId" in req ? this.drafts.find((d) => d.id === req.draftId) : undefined;
    const source = "draftId" in req ? draft?.ruleset : req.ruleset;
    const basedOn = "draftId" in req ? draft?.basedOn : req.basedOn;
    if (!source) {
      throw new Error("Not found");
    }
    const problem = rulesetProblem(source);
    if (problem) {
      throw new Error(problem);
    }
    const base = this.rulesets.find((r) => r.code === basedOn && r.ruleset.by === "Mock User");
    const version = base
      ? Math.max(...this.rulesets.filter((r) => r.ruleset.name === base.ruleset.name).map((r) => r.ruleset.version)) + 1
      : 1;
    const code = Array.from({ length: 6 }, () => "BCDFGHJKLMNPQRSTVWXZ"[Math.floor(Math.random() * 20)]).join("");
    const view: RulesetView = {
      code,
      ruleset: { ...normalizeRuleset(source), version, by: "Mock User" },
      hash: "0".repeat(64),
      createdAt: new Date().toISOString(),
      newer: null,
      link: `https://brawlonline.net/r/${code}`,
      hidden: false,
      games: 0,
    };
    this.rulesets.push(view);
    this.drafts = this.drafts.filter((d) => d !== draft);
    return view;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async setRulesetHidden(code: string, hidden: boolean): Promise<void> {
    this.rulesets = this.rulesets.map((r) => (r.code === code ? { ...r, hidden } : r));
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async saveRulesetDraft(req: RulesetDraftRequest, id?: string): Promise<RulesetDraft> {
    const now = new Date().toISOString();
    const old = this.drafts.find((d) => d.id === id);
    const draft: RulesetDraft = {
      id: old?.id ?? `draft-${Date.now()}`,
      basedOn: req.basedOn ?? null,
      ruleset: { ...req.ruleset, version: 0, by: "" },
      problem: rulesetProblem(req.ruleset),
      createdAt: old?.createdAt ?? now,
      updatedAt: now,
    };
    this.drafts = [draft, ...this.drafts.filter((d) => d.id !== draft.id)];
    return draft;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async deleteRulesetDraft(id: string): Promise<void> {
    this.drafts = this.drafts.filter((d) => d.id !== id);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async fetchRememberedRuleset(): Promise<RulesetView | null> {
    return this.remembered ? this.fetchRuleset(this.remembered) : null;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async setRememberedRuleset(code: string | null): Promise<RulesetView | null> {
    if (code === null) {
      this.remembered = null;
      return null;
    }
    const view = await this.fetchRuleset(code);
    this.remembered = view.code;
    this.recent = [view.code, ...this.recent.filter((c) => c !== view.code)].slice(0, 10);
    return view;
  }

  private _withNewer(r: RulesetView): RulesetView {
    const newer = this.rulesets
      .filter(
        (o) =>
          o.ruleset.name === r.ruleset.name &&
          o.ruleset.by === r.ruleset.by &&
          o.ruleset.version > r.ruleset.version &&
          !o.hidden,
      )
      .sort((a, b) => b.ruleset.version - a.ruleset.version)[0];
    return { ...r, newer: newer ? { code: newer.code, version: newer.ruleset.version } : null };
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
