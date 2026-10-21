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
  status: "waiting" | "starting" | "in-game";
  /** An open slot is empty. */
  joinable: boolean;
};

export type SocialPlayer = {
  uid: string;
  displayName: string;
  /** `ABCD#123` */
  connectCode: string;
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
  | { type: "banned" };

export type SendResult = { ok: true } | { ok: false; message: string };

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
  markRead(uid: string): Promise<void>;
  onStateChange(handle: (state: SocialState) => void): () => void;
  onEvent(handle: (event: SocialEvent) => void): () => void;
}
