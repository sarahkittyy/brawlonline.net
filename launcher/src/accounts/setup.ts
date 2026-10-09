import { appVersion } from "@common/constants";
import { PRODUCT_SLUG } from "@common/product";
import { app, safeStorage } from "electron";
import electronLog from "electron-log";

import { AccountsManager } from "./accounts_manager";
import { AccountsHttpClient } from "./client";
import { resolveServiceUrls } from "./config";
import {
  ipc_accountsHasSession,
  ipc_accountsInitNetplay,
  ipc_accountsLeaderboard,
  ipc_accountsLogin,
  ipc_accountsLogout,
  ipc_accountsMatchHistory,
  ipc_accountsMe,
  ipc_accountsPublicUser,
  ipc_accountsRename,
  ipc_accountsRequestPasswordReset,
  ipc_accountsResendVerification,
  ipc_accountsServiceUrls,
  ipc_accountsSignUp,
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
  ipc_accountsServiceUrls.main!.handle(async () => urls);

  return { accountsManager: manager, serviceUrls: urls };
}
