import type { ServiceUrls } from "@common/product";
import { _, makeEndpoint } from "utils/ipc";

import type { AccountsMe, AccountsPublicUser, AccountsResult, SignUpRequest } from "./types";

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

export const ipc_accountsAcceptRules = makeEndpoint.main(
  "accounts_acceptRules",
  <{ uid: string; num: number }>_,
  <R<AccountsMe>>_,
);

export const ipc_accountsPublicUser = makeEndpoint.main(
  "accounts_publicUser",
  <{ uid: string }>_,
  <R<AccountsPublicUser>>_,
);

export const ipc_accountsServiceUrls = makeEndpoint.main(
  "accounts_serviceUrls",
  <Record<never, never>>_,
  <ServiceUrls>_,
);
