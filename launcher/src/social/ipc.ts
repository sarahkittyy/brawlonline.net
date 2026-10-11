import type { EmptyPayload, SuccessPayload } from "utils/ipc";
import { _, makeEndpoint } from "utils/ipc";

import type { InviteResult, SendResult, SocialEvent, SocialState } from "./types";

export const ipc_socialConnect = makeEndpoint.main("social_connect", <{ uid: string }>_, <SuccessPayload>_);

export const ipc_socialDisconnect = makeEndpoint.main("social_disconnect", <EmptyPayload>_, <SuccessPayload>_);

export const ipc_socialGetState = makeEndpoint.main("social_getState", <EmptyPayload>_, <{ state: SocialState }>_);

export const ipc_socialSendMessage = makeEndpoint.main(
  "social_sendMessage",
  <{ to: string; text: string }>_,
  <{ result: SendResult }>_,
);

export const ipc_socialInvite = makeEndpoint.main("social_invite", <{ to: string }>_, <{ result: InviteResult }>_);

export const ipc_socialAnswerInvite = makeEndpoint.main(
  "social_answerInvite",
  <{ id: string; accept: boolean }>_,
  <{ result: SendResult }>_,
);

export const ipc_socialDismissInvite = makeEndpoint.main("social_dismissInvite", <{ id: string }>_, <SuccessPayload>_);

export const ipc_socialSetIgnored = makeEndpoint.main(
  "social_setIgnored",
  <{ uid: string; ignored: boolean }>_,
  <SuccessPayload>_,
);

export const ipc_socialMarkRead = makeEndpoint.main("social_markRead", <{ uid: string }>_, <SuccessPayload>_);

export const ipc_socialStateChangedEvent = makeEndpoint.renderer("social_stateChanged", <{ state: SocialState }>_);

export const ipc_socialEvent = makeEndpoint.renderer("social_event", <{ event: SocialEvent }>_);
