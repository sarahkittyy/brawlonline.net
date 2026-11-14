/**
 * The launcher's social features (docs/social-protocol.md), as the renderer sees them: who is
 * online, direct message conversations, invites. The main process (social_client.ts) holds the
 * connection, the keys and this state, and sends the renderer a fresh copy on every change.
 */

export type RoomMode = "1v1" | "ffa" | "teams";

/** The player's own room (from mm, through accounts), or the inviter's in an invite. */
export type SocialRoom = {
  code: string;
  players: number;
  openSlots: number;
  mode: RoomMode;
  /** `searching`: a 2v2 party searching the queue. */
  status: "waiting" | "starting" | "in-game" | "searching";
  /** An open slot is empty. */
  joinable: boolean;
  /** A 2v2 party (docs/design/2v2.md), not a room: invites are to "your 2v2". */
  twos?: boolean;
};

export type SocialPlayer = {
  uid: string;
  displayName: string;
  /** `ABCD#123` */
  connectCode: string;
  /** Their flag code, when they show it beside their name (docs/design/profiles.md). */
  country?: string;
  /** Their primary main's character key, when they show it. */
  main?: string;
};

export type ChatLine = {
  id: string;
  /** Sent by this launcher (else by the conversation's other player). */
  own: boolean;
  text: string;
  /** Unix ms. */
  at: number;
};

export type Conversation = {
  player: SocialPlayer;
  lines: ChatLine[];
  /** Lines from the other player not yet seen. Ignored players' lines never count. */
  unread: number;
  online: boolean;
};

export type Invite = {
  id: string;
  from: SocialPlayer;
  kind: "room" | "direct";
  /** `kind: "room"`: the room to join. */
  room?: SocialRoom;
  /** Unix ms. */
  receivedAt: number;
};

/** A friend, or a player in a friend request (docs/design/friends.md). */
export type Friend = SocialPlayer & {
  /** ISO 8601: when they became friends, or when the request was sent. */
  since: string;
};

/** `GET /v1/friends`, as the main process last read it. */
export type FriendsState = {
  /** Read at least once since connecting. */
  loaded: boolean;
  /** By name, online or not. */
  friends: Friend[];
  /** Requests to the player, newest first. */
  incoming: Friend[];
  /** The player's own requests waiting for an answer, newest first. */
  outgoing: Friend[];
};

export type SocialStatus =
  /** Not logged in, or online play not set up. */
  | "off"
  | "connecting"
  | "online"
  /** Lost; retrying. */
  | "reconnecting";

export type SocialState = {
  status: SocialStatus;
  /** Why the list is not there (shown while `status` isn't `online`). */
  message?: string;
  you?: string;
  /** Online players, the player excluded, by name. */
  online: SocialPlayer[];
  room: SocialRoom | null;
  /** Ignored uids (shared with the game's chat: `<User>/Online/chat-hidden.json`). */
  ignored: string[];
  /** By the other player's uid. */
  conversations: Record<string, Conversation>;
  invites: Invite[];
  friends: FriendsState;
  /**
   * Friend requests that arrived while the launcher was open, shown as pop-ups until answered or
   * closed (they don't time out; a closed one stays in Friends > Requests).
   */
  requestPopups: Friend[];
};

/** Something the renderer may want to make a sound or a toast for. */
export type SocialEvent =
  | { type: "message"; uid: string }
  | { type: "invite"; invite: Invite }
  | { type: "invite-answer"; player: SocialPlayer; kind: "room" | "direct"; accept: boolean }
  | { type: "replaced" }
  /** The account changed on the server (an admin rename): read it again. */
  | { type: "account" }
  /** The account was banned: the stream is over, and the session too. */
  | { type: "banned" }
  /** Someone asked the player to be friends (also in `requestPopups`). */
  | { type: "friend-request"; player: Friend }
  /** Someone accepted the player's friend request. */
  | { type: "friend-accepted"; player: Friend };

export type SendResult = { ok: true } | { ok: false; message: string };

/**
 * A friend request sent: `sent` waits for them; `friends`: they had asked too, so they are friends
 * now (or already were).
 */
export type FriendRequestResult =
  | { ok: true; status: "sent" | "friends"; player: Friend }
  | { ok: false; message: string };

export type InviteResult = { ok: true; kind: "room" | "direct" } | { ok: false; message: string };

export interface SocialApi {
  /** Connects for this account (the active one); a different uid replaces the connection. */
  connect(uid: string): Promise<void>;
  disconnect(): Promise<void>;
  getState(): Promise<SocialState>;
  sendMessage(to: string, text: string): Promise<SendResult>;
  invite(to: string): Promise<InviteResult>;
  /** Answers an invite (and drops it from the list). */
  answerInvite(id: string, accept: boolean): Promise<SendResult>;
  /** Drops an invite without telling the inviter (the notification's X). */
  dismissInvite(id: string): Promise<void>;
  setIgnored(uid: string, ignored: boolean): Promise<void>;
  /** `to`: a connect code, a profile address or link, or a uid. */
  sendFriendRequest(to: string): Promise<FriendRequestResult>;
  acceptFriend(uid: string): Promise<SendResult>;
  /** Hides their request (they aren't told). */
  declineFriend(uid: string): Promise<SendResult>;
  /** Takes back the player's own request. */
  cancelFriendRequest(uid: string): Promise<SendResult>;
  removeFriend(uid: string): Promise<SendResult>;
  /** Closes a request's pop-up (the request stays in Friends > Requests). */
  dismissRequestPopup(uid: string): Promise<void>;
  markRead(uid: string): Promise<void>;
  onStateChange(handle: (state: SocialState) => void): () => void;
  onEvent(handle: (event: SocialEvent) => void): () => void;
}
