import { Preconditions } from "@common/preconditions";
import type { PlayKey } from "@dolphin/types";

import type { AuthService } from "../auth/types";
import { delayAndMaybeError } from "../utils";
import type { BackendService, RankedProfile, UserData } from "./types";
import { Rank } from "./types";

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
  rank: Rank.GRANDMASTER,
  rating: 9001,
};

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
