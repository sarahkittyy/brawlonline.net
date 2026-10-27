import { appVersion } from "@common/constants";
import { PRODUCT_SLUG } from "@common/product";
import { app, safeStorage } from "electron";
import electronLog from "electron-log";

import { AccountsManager } from "./accounts_manager";
import { AccountsHttpClient } from "./client";
import { resolveServiceUrls } from "./config";
import {
  ipc_accountsBrowseRulesets,
  ipc_accountsCreateRulesetDraft,
  ipc_accountsDeleteRulesetDraft,
  ipc_accountsHasSession,
  ipc_accountsInitNetplay,
  ipc_accountsLeaderboard,
  ipc_accountsLogin,
  ipc_accountsLogout,
  ipc_accountsMatchHistory,
  ipc_accountsMe,
  ipc_accountsMyRulesets,
  ipc_accountsPublicUser,
  ipc_accountsPublishRuleset,
  ipc_accountsRecentRulesets,
  ipc_accountsRememberedRuleset,
  ipc_accountsRename,
  ipc_accountsRequestPasswordReset,
  ipc_accountsResendVerification,
  ipc_accountsRooms,
  ipc_accountsRuleset,
  ipc_accountsServiceUrls,
  ipc_accountsSetRememberedRuleset,
  ipc_accountsSetRoomName,
  ipc_accountsSetRulesetHidden,
  ipc_accountsSignUp,
  ipc_accountsUpdateRulesetDraft,
} from "./ipc";
import { SessionStore } from "./session_store";

const log = electronLog.scope("accounts");

export default function setupAccountsIpc() {
  const urls = resolveServiceUrls();
  log.info(`Accounts API: ${urls.accountsApi}`);
  const client = new AccountsHttpClient(urls.accountsApi, `${PRODUCT_SLUG}-launcher/${appVersion}`);
  const sessions = new SessionStore(SessionStore.defaultPath(app.getPath("userData")), safeStorage);
  const manager = new AccountsManager(client, sessions);

  ipc_accountsSignUp.main!.handle(async ({ req }) => await manager.signUp(req));
  ipc_accountsLogin.main!.handle(async ({ email, password }) => await manager.login(email, password));
  ipc_accountsLogout.main!.handle(async ({ uid }) => await manager.logout(uid));
  ipc_accountsHasSession.main!.handle(async ({ uid }) => await manager.hasSession(uid));
  ipc_accountsMe.main!.handle(async ({ uid }) => await manager.me(uid));
  ipc_accountsResendVerification.main!.handle(async ({ uid }) => await manager.resendVerification(uid));
  ipc_accountsRequestPasswordReset.main!.handle(async ({ email }) => manager.requestPasswordReset(email));
  ipc_accountsInitNetplay.main!.handle(async ({ uid, codeStart }) => manager.initNetplay(uid, codeStart));
  ipc_accountsRename.main!.handle(async ({ uid, displayName }) => await manager.rename(uid, displayName));
  ipc_accountsPublicUser.main!.handle(async ({ uid }) => await manager.publicUser(uid));
  ipc_accountsLeaderboard.main!.handle(async ({ limit, after }) => await manager.leaderboard({ limit, after }));
  ipc_accountsMatchHistory.main!.handle(
    async ({ uid, mode, limit, before }) => await manager.matchHistory(uid, { mode, limit, before }),
  );
  ipc_accountsRooms.main!.handle(async ({ uid }) => await manager.rooms(uid));
  ipc_accountsSetRoomName.main!.handle(async ({ uid, kind, name }) => await manager.setRoomName(uid, kind, name));
  ipc_accountsRuleset.main!.handle(async ({ code }) => await manager.ruleset(code));
  ipc_accountsBrowseRulesets.main!.handle(
    async ({ q, limit, after }) => await manager.browseRulesets({ q, limit, after }),
  );
  ipc_accountsMyRulesets.main!.handle(async ({ uid }) => await manager.myRulesets(uid));
  ipc_accountsRecentRulesets.main!.handle(async ({ uid }) => await manager.recentRulesets(uid));
  ipc_accountsPublishRuleset.main!.handle(async ({ uid, req }) => await manager.publishRuleset(uid, req));
  ipc_accountsSetRulesetHidden.main!.handle(
    async ({ uid, code, hidden }) => await manager.setRulesetHidden(uid, code, hidden),
  );
  ipc_accountsCreateRulesetDraft.main!.handle(async ({ uid, req }) => await manager.createRulesetDraft(uid, req));
  ipc_accountsUpdateRulesetDraft.main!.handle(
    async ({ uid, id, req }) => await manager.updateRulesetDraft(uid, id, req),
  );
  ipc_accountsDeleteRulesetDraft.main!.handle(async ({ uid, id }) => await manager.deleteRulesetDraft(uid, id));
  ipc_accountsRememberedRuleset.main!.handle(async ({ uid }) => await manager.rememberedRuleset(uid));
  ipc_accountsSetRememberedRuleset.main!.handle(async ({ uid, code }) => await manager.setRememberedRuleset(uid, code));
  ipc_accountsServiceUrls.main!.handle(async () => urls);

  return { accountsManager: manager, serviceUrls: urls };
}
