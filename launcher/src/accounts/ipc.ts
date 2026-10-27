import type { ServiceUrls } from "@common/product";
import { _, makeEndpoint } from "utils/ipc";

import type {
  AccountsMe,
  AccountsPublicUser,
  AccountsResult,
  LeaderboardPage,
  MatchHistoryFilter,
  MatchHistoryPage,
  MyRulesets,
  PublishRulesetRequest,
  RecentRuleset,
  RoomKind,
  RoomList,
  RulesetDraft,
  RulesetDraftRequest,
  RulesetPage,
  RulesetView,
  SignUpRequest,
} from "./types";

type R<T> = AccountsResult<T>;

export const ipc_accountsSignUp = makeEndpoint.main("accounts_signUp", <{ req: SignUpRequest }>_, <R<AccountsMe>>_);

export const ipc_accountsLogin = makeEndpoint.main(
  "accounts_login",
  <{ email: string; password: string }>_,
  <R<AccountsMe>>_,
);

export const ipc_accountsLogout = makeEndpoint.main("accounts_logout", <{ uid: string }>_, <R<null>>_);

export const ipc_accountsHasSession = makeEndpoint.main("accounts_hasSession", <{ uid: string }>_, <R<boolean>>_);

export const ipc_accountsMe = makeEndpoint.main("accounts_me", <{ uid: string }>_, <R<AccountsMe>>_);

export const ipc_accountsResendVerification = makeEndpoint.main(
  "accounts_resendVerification",
  <{ uid: string }>_,
  <R<null>>_,
);

export const ipc_accountsRequestPasswordReset = makeEndpoint.main(
  "accounts_requestPasswordReset",
  <{ email: string }>_,
  <R<null>>_,
);

export const ipc_accountsInitNetplay = makeEndpoint.main(
  "accounts_initNetplay",
  <{ uid: string; codeStart: string }>_,
  <R<AccountsMe>>_,
);

export const ipc_accountsRename = makeEndpoint.main(
  "accounts_rename",
  <{ uid: string; displayName: string }>_,
  <R<AccountsMe>>_,
);

export const ipc_accountsPublicUser = makeEndpoint.main(
  "accounts_publicUser",
  <{ uid: string }>_,
  <R<AccountsPublicUser>>_,
);

export const ipc_accountsLeaderboard = makeEndpoint.main(
  "accounts_leaderboard",
  <{ limit?: number; after?: string }>_,
  <R<LeaderboardPage>>_,
);

export const ipc_accountsMatchHistory = makeEndpoint.main(
  "accounts_matchHistory",
  <{ uid: string; mode: MatchHistoryFilter; limit?: number; before?: string }>_,
  <R<MatchHistoryPage>>_,
);

export const ipc_accountsRooms = makeEndpoint.main("accounts_rooms", <{ uid: string }>_, <R<RoomList>>_);

export const ipc_accountsSetRoomName = makeEndpoint.main(
  "accounts_setRoomName",
  <{ uid: string; kind: RoomKind; name: string | null }>_,
  <R<string | null>>_,
);

export const ipc_accountsRuleset = makeEndpoint.main("accounts_ruleset", <{ code: string }>_, <R<RulesetView>>_);

export const ipc_accountsBrowseRulesets = makeEndpoint.main(
  "accounts_browseRulesets",
  <{ q?: string; limit?: number; after?: string }>_,
  <R<RulesetPage>>_,
);

export const ipc_accountsMyRulesets = makeEndpoint.main("accounts_myRulesets", <{ uid: string }>_, <R<MyRulesets>>_);

export const ipc_accountsRecentRulesets = makeEndpoint.main(
  "accounts_recentRulesets",
  <{ uid: string }>_,
  <R<RecentRuleset[]>>_,
);

export const ipc_accountsPublishRuleset = makeEndpoint.main(
  "accounts_publishRuleset",
  <{ uid: string; req: PublishRulesetRequest }>_,
  <R<RulesetView>>_,
);

export const ipc_accountsSetRulesetHidden = makeEndpoint.main(
  "accounts_setRulesetHidden",
  <{ uid: string; code: string; hidden: boolean }>_,
  <R<null>>_,
);

export const ipc_accountsCreateRulesetDraft = makeEndpoint.main(
  "accounts_createRulesetDraft",
  <{ uid: string; req: RulesetDraftRequest }>_,
  <R<RulesetDraft>>_,
);

export const ipc_accountsUpdateRulesetDraft = makeEndpoint.main(
  "accounts_updateRulesetDraft",
  <{ uid: string; id: string; req: RulesetDraftRequest }>_,
  <R<RulesetDraft>>_,
);

export const ipc_accountsDeleteRulesetDraft = makeEndpoint.main(
  "accounts_deleteRulesetDraft",
  <{ uid: string; id: string }>_,
  <R<null>>_,
);

export const ipc_accountsRememberedRuleset = makeEndpoint.main(
  "accounts_rememberedRuleset",
  <{ uid: string }>_,
  <R<RulesetView | null>>_,
);

export const ipc_accountsSetRememberedRuleset = makeEndpoint.main(
  "accounts_setRememberedRuleset",
  <{ uid: string; code: string | null }>_,
  <R<RulesetView | null>>_,
);

export const ipc_accountsServiceUrls = makeEndpoint.main(
  "accounts_serviceUrls",
  <Record<never, never>>_,
  <ServiceUrls>_,
);
