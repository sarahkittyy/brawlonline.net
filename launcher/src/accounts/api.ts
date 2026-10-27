/* eslint-disable import/no-default-export */
import {
  ipc_accountsBrowseRulesets,
  ipc_accountsChangeEmail,
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
  async changeEmail(uid, email) {
    return unwrap(await ipc_accountsChangeEmail.renderer!.trigger({ uid, email }));
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
  async rooms(uid) {
    return unwrap(await ipc_accountsRooms.renderer!.trigger({ uid }));
  },
  async setRoomName(uid, kind, name) {
    return unwrap(await ipc_accountsSetRoomName.renderer!.trigger({ uid, kind, name }));
  },
  async ruleset(code) {
    return unwrap(await ipc_accountsRuleset.renderer!.trigger({ code }));
  },
  async browseRulesets(query) {
    return unwrap(await ipc_accountsBrowseRulesets.renderer!.trigger(query));
  },
  async myRulesets(uid) {
    return unwrap(await ipc_accountsMyRulesets.renderer!.trigger({ uid }));
  },
  async recentRulesets(uid) {
    return unwrap(await ipc_accountsRecentRulesets.renderer!.trigger({ uid }));
  },
  async publishRuleset(uid, req) {
    return unwrap(await ipc_accountsPublishRuleset.renderer!.trigger({ uid, req }));
  },
  async setRulesetHidden(uid, code, hidden) {
    unwrap(await ipc_accountsSetRulesetHidden.renderer!.trigger({ uid, code, hidden }));
  },
  async createRulesetDraft(uid, req) {
    return unwrap(await ipc_accountsCreateRulesetDraft.renderer!.trigger({ uid, req }));
  },
  async updateRulesetDraft(uid, id, req) {
    return unwrap(await ipc_accountsUpdateRulesetDraft.renderer!.trigger({ uid, id, req }));
  },
  async deleteRulesetDraft(uid, id) {
    unwrap(await ipc_accountsDeleteRulesetDraft.renderer!.trigger({ uid, id }));
  },
  async rememberedRuleset(uid) {
    return unwrap(await ipc_accountsRememberedRuleset.renderer!.trigger({ uid }));
  },
  async setRememberedRuleset(uid, code) {
    return unwrap(await ipc_accountsSetRememberedRuleset.renderer!.trigger({ uid, code }));
  },
  async getServiceUrls() {
    const { result } = await ipc_accountsServiceUrls.renderer!.trigger({});
    return result;
  },
};

export default accountsApi;
