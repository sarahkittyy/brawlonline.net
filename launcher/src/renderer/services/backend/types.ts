import type { PlayKey } from "@dolphin/types";

/** The ranked rating: an Elo number on Slippi's scale (server/crates/common/src/ranked.rs). No tiers. */
export type RankedProfile = {
  rating: number;
  /** Rated ranked sets (Slippi's `ratingUpdateCount`). */
  setsPlayed: number;
};

export type UserData = {
  playKey?: PlayKey;
  rankedNetplayProfile?: RankedProfile;
};

export interface BackendService {
  validateUserId(userId: string): Promise<{ displayName: string; connectCode: string }>;
  fetchUserData(): Promise<UserData | undefined>;
  fetchRankedNetplayProfile(userId: string): Promise<RankedProfile | undefined>;
  assertPlayKey(playKey: PlayKey): Promise<void>;
  deletePlayKey(): Promise<void>;
  changeDisplayName(name: string): Promise<void>;
  initializeNetplay(codeStart: string): Promise<void>;
}
