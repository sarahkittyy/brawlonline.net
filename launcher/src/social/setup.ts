import type { AccountsManager } from "@accounts/accounts_manager";
import { appVersion } from "@common/constants";
import { PRODUCT_SLUG } from "@common/product";
import type { DolphinManager } from "@dolphin/manager";

import {
  ipc_socialAnswerInvite,
  ipc_socialConnect,
  ipc_socialDisconnect,
  ipc_socialDismissInvite,
  ipc_socialEvent,
  ipc_socialGetState,
  ipc_socialInvite,
  ipc_socialMarkRead,
  ipc_socialSendMessage,
  ipc_socialSetIgnored,
  ipc_socialStateChangedEvent,
} from "./ipc";
import { SocialClient } from "./social_client";

export default function setupSocialIpc({
  accountsManager,
  accountsApi,
  dolphinManager,
}: {
  accountsManager: AccountsManager;
  accountsApi: string;
  dolphinManager: DolphinManager;
}) {
  const client = new SocialClient({
    baseUrl: accountsApi.replace(/\/+$/, ""),
    userAgent: `${PRODUCT_SLUG}-launcher/${appVersion}`,
    getToken: (uid) => accountsManager.sessionToken(uid),
    onlineDir: () => dolphinManager.netplayOnlineDir(),
  });
  client.onState((state) => void ipc_socialStateChangedEvent.main!.trigger({ state }));
  client.onEvent((event) => void ipc_socialEvent.main!.trigger({ event }));

  ipc_socialConnect.main!.handle(async ({ uid }) => {
    await client.connect(uid);
    return { success: true };
  });
  ipc_socialDisconnect.main!.handle(async () => {
    client.disconnect();
    return { success: true };
  });
  ipc_socialGetState.main!.handle(async () => ({ state: client.getState() }));
  ipc_socialSendMessage.main!.handle(async ({ to, text }) => ({ result: await client.sendMessage(to, text) }));
  ipc_socialInvite.main!.handle(async ({ to }) => ({ result: await client.invite(to) }));
  ipc_socialAnswerInvite.main!.handle(async ({ id, accept }) => ({ result: await client.answerInvite(id, accept) }));
  ipc_socialDismissInvite.main!.handle(async ({ id }) => {
    client.dismissInvite(id);
    return { success: true };
  });
  ipc_socialSetIgnored.main!.handle(async ({ uid, ignored }) => {
    await client.setIgnored(uid, ignored);
    return { success: true };
  });
  ipc_socialMarkRead.main!.handle(async ({ uid }) => {
    client.markRead(uid);
    return { success: true };
  });
  return client;
}
