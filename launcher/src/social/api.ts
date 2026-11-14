/* eslint-disable import/no-default-export */
import {
  ipc_socialAnswerInvite,
  ipc_socialConnect,
  ipc_socialDisconnect,
  ipc_socialDismissInvite,
  ipc_socialDismissRequestPopup,
  ipc_socialEvent,
  ipc_socialFriendAction,
  ipc_socialGetState,
  ipc_socialInvite,
  ipc_socialMarkRead,
  ipc_socialSendFriendRequest,
  ipc_socialSendMessage,
  ipc_socialSetIgnored,
  ipc_socialStateChangedEvent,
} from "./ipc";
import type { SocialApi } from "./types";

const socialApi: SocialApi = {
  async connect(uid) {
    await ipc_socialConnect.renderer!.trigger({ uid });
  },
  async disconnect() {
    await ipc_socialDisconnect.renderer!.trigger({});
  },
  async getState() {
    const { result } = await ipc_socialGetState.renderer!.trigger({});
    return result.state;
  },
  async sendMessage(to, text) {
    const { result } = await ipc_socialSendMessage.renderer!.trigger({ to, text });
    return result.result;
  },
  async invite(to) {
    const { result } = await ipc_socialInvite.renderer!.trigger({ to });
    return result.result;
  },
  async answerInvite(id, accept) {
    const { result } = await ipc_socialAnswerInvite.renderer!.trigger({ id, accept });
    return result.result;
  },
  async dismissInvite(id) {
    await ipc_socialDismissInvite.renderer!.trigger({ id });
  },
  async setIgnored(uid, ignored) {
    await ipc_socialSetIgnored.renderer!.trigger({ uid, ignored });
  },
  async sendFriendRequest(to) {
    const { result } = await ipc_socialSendFriendRequest.renderer!.trigger({ to });
    return result.result;
  },
  async acceptFriend(uid) {
    const { result } = await ipc_socialFriendAction.renderer!.trigger({ action: "accept", uid });
    return result.result;
  },
  async declineFriend(uid) {
    const { result } = await ipc_socialFriendAction.renderer!.trigger({ action: "decline", uid });
    return result.result;
  },
  async cancelFriendRequest(uid) {
    const { result } = await ipc_socialFriendAction.renderer!.trigger({ action: "cancel", uid });
    return result.result;
  },
  async removeFriend(uid) {
    const { result } = await ipc_socialFriendAction.renderer!.trigger({ action: "remove", uid });
    return result.result;
  },
  async dismissRequestPopup(uid) {
    await ipc_socialDismissRequestPopup.renderer!.trigger({ uid });
  },
  async markRead(uid) {
    await ipc_socialMarkRead.renderer!.trigger({ uid });
  },
  onStateChange(handle) {
    const { destroy } = ipc_socialStateChangedEvent.renderer!.handle(async ({ state }) => {
      handle(state);
    });
    return destroy;
  },
  onEvent(handle) {
    const { destroy } = ipc_socialEvent.renderer!.handle(async ({ event }) => {
      handle(event);
    });
    return destroy;
  },
};

export default socialApi;
