/* eslint-disable import/no-default-export */
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
import type { AccountsApi, AccountsResult } from "./types";
import { AccountsError } from "./types";

function unwrap<T>(res: { result: AccountsResult<T> }): T {
  const r = res.result;
  if (r.ok) {
    return r.value;
  }
  throw new AccountsError(r.error.code, r.error.message, r.error.status, r.error.retryAfter);
}

const accountsApi: AccountsApi = {
  async signUp(req) {
    return unwrap(await ipc_accountsSignUp.renderer!.trigger({ req }));
  },
  async login(email, password) {
    return unwrap(await ipc_accountsLogin.renderer!.trigger({ email, password }));
  },
  async logout(uid) {
    unwrap(await ipc_accountsLogout.renderer!.trigger({ uid }));
  },
  async hasSession(uid) {
    return unwrap(await ipc_accountsHasSession.renderer!.trigger({ uid }));
  },
  async me(uid) {
    return unwrap(await ipc_accountsMe.renderer!.trigger({ uid }));
  },
  async resendVerificationEmail(uid) {
    unwrap(await ipc_accountsResendVerification.renderer!.trigger({ uid }));
  },
  async requestPasswordReset(email) {
    unwrap(await ipc_accountsRequestPasswordReset.renderer!.trigger({ email }));
  },
  async initNetplay(uid, codeStart) {
    return unwrap(await ipc_accountsInitNetplay.renderer!.trigger({ uid, codeStart }));
  },
  async rename(uid, displayName) {
    return unwrap(await ipc_accountsRename.renderer!.trigger({ uid, displayName }));
  },
  async publicUser(uid) {
    return unwrap(await ipc_accountsPublicUser.renderer!.trigger({ uid }));
  },
  async leaderboard(query) {
    return unwrap(await ipc_accountsLeaderboard.renderer!.trigger(query));
  },
  async matchHistory(uid, query) {
    return unwrap(await ipc_accountsMatchHistory.renderer!.trigger({ uid, ...query }));
  },
  async getServiceUrls() {
    const { result } = await ipc_accountsServiceUrls.renderer!.trigger({});
    return result;
  },
};

export default accountsApi;
