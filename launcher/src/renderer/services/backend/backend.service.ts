import type { AccountsMe, AccountsPublicUser } from "@accounts/types";
import { Preconditions } from "@common/preconditions";
import type { DolphinService, PlayKey } from "@dolphin/types";
import log from "electron-log";

import type { AuthService } from "../auth/types";
import type { BackendService, RankedProfile, UserData } from "./types";

/**
 * The online backend, over our accounts API (server/crates/accounts). Replaces
 * Slippi's GraphQL client. Operation mapping (backend-design.md section 2.4):
 *
 * | Slippi GraphQL                 | accounts API                      |
 * |--------------------------------|-----------------------------------|
 * | getUser (incl. private.playKey)| GET /v1/me                        |
 * | validateUserIdQuery            | GET /user/{uid}                   |
 * | rankedNetplayProfile           | GET /user/{uid} `rank`            |
 * | userRename                     | POST /v1/me/rename                |
 * | userInitNetplay                | POST /v1/me/netplay               |
 * | getLatestDolphin.version       | `latestVersion` in /v1/me         |
 */

function mapRankedProfile(user: AccountsPublicUser): RankedProfile {
  return { rating: user.rank.ratingOrdinal ?? 0, setsPlayed: user.rank.ratingUpdateCount ?? 0 };
}

/** user.json content in Slippi's key order: uid, playKey, connectCode, displayName, latestVersion. */
export function playKeyFromAccount(me: AccountsMe): PlayKey | undefined {
  const json = me.userJson;
  if (!json) {
    return undefined;
  }
  return {
    uid: json.uid,
    playKey: json.playKey,
    connectCode: json.connectCode,
    displayName: json.displayName,
    latestVersion: json.latestVersion,
  };
}

class AccountsBackendClient implements BackendService {
  constructor(private readonly authService: AuthService, private readonly dolphinService: DolphinService) {}

  private get _api() {
    return window.electron.accounts;
  }

  private _activeUid(): string {
    const user = this.authService.getCurrentUser();
    Preconditions.checkExists(user, "User is not logged in");
    return user.uid;
  }

  async validateUserId(userId: string): Promise<{ displayName: string; connectCode: string }> {
    const user = await this._api.publicUser(userId);
    if (user.connectCode) {
      return { connectCode: user.connectCode, displayName: user.displayName ?? "" };
    }
    throw new Error("No user with that ID");
  }

  async fetchUserData(): Promise<UserData | undefined> {
    this._activeUid();
    const me = await this.authService.getMultiAccountService().refreshActiveUser();
    if (!me) {
      return undefined;
    }

    let rankedNetplayProfile: RankedProfile | undefined;
    try {
      rankedNetplayProfile = mapRankedProfile(await this._api.publicUser(me.uid));
    } catch (err) {
      log.warn("Could not fetch the ranked profile", err);
    }

    return {
      // If we don't have a connect code or play key, this is undefined so that the
      // logic handling it asks the user to set them up.
      playKey: playKeyFromAccount(me),
      rankedNetplayProfile,
    };
  }

  async fetchRankedNetplayProfile(userId: string): Promise<RankedProfile | undefined> {
    return mapRankedProfile(await this._api.publicUser(userId));
  }

  async assertPlayKey(playKey: PlayKey) {
    const playKeyExists = await this.dolphinService.checkPlayKeyExists(playKey);
    if (playKeyExists) {
      return;
    }

    await this.dolphinService.storePlayKeyFile(playKey);
  }

  async deletePlayKey(): Promise<void> {
    await this.dolphinService.removePlayKeyFile();
  }

  async changeDisplayName(name: string) {
    this._activeUid();
    await this.authService.updateDisplayName(name);
    const user = this.authService.getCurrentUser();
    if (user?.displayName !== name) {
      throw new Error("Could not change name.");
    }
  }

  async initializeNetplay(codeStart: string): Promise<void> {
    const uid = this._activeUid();
    const me = await this._api.initNetplay(uid, codeStart);
    this.authService.getMultiAccountService().setUserRecord(me);
  }
}

export default function createBackendClient(
  authService: AuthService,
  dolphinService: DolphinService,
  _clientVersion?: string,
): BackendService {
  return new AccountsBackendClient(authService, dolphinService);
}
