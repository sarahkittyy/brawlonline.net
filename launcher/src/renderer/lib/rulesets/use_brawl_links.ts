import type { BrawlLink } from "@common/brawl_link";
import { useQueryClient } from "@tanstack/react-query";
import log from "electron-log";
import { useCallback, useEffect } from "react";
import { useNavigate } from "react-router-dom";

import { useAccount } from "@/lib/hooks/use_account";
import { useLoginModal } from "@/lib/hooks/use_login_modal";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useCreateRoom } from "@/lib/social/use_create_room";
import { HomeRoutes } from "@/pages/home/home_routes";
import { useServices } from "@/services";

import { RulesetLinkMessages as Messages } from "./use_brawl_links.messages";
import { rememberedRulesetKey, rulesetTitle } from "./use_remembered_ruleset";

/**
 * Acts on `brawlonline://` links (src/common/brawl_link.ts) while the main page is up:
 * `room/create?ruleset=<code>` makes the code the account's remembered ruleset (Home's box shows
 * it from then on) and creates a room with it, as Create room does. Links that came in before the
 * page was up are taken when it mounts.
 */
export const useBrawlLinks = () => {
  const { backendService } = useServices();
  const queryClient = useQueryClient();
  const navigate = useNavigate();
  const openLogin = useLoginModal((s) => s.openModal);
  const { showError, showInfo, showWarning } = useToasts();
  const { create } = useCreateRoom();

  const handle = useCallback(
    async (link: BrawlLink) => {
      const uid = useAccount.getState().user?.uid;
      if (!uid) {
        showWarning(Messages.logInFirst());
        openLogin();
        return;
      }
      navigate(HomeRoutes.overview());
      let code: string | undefined;
      try {
        if (link.ruleset) {
          const view = await backendService.setRememberedRuleset(link.ruleset);
          queryClient.setQueryData(rememberedRulesetKey(uid), view);
          code = view?.code;
          if (view) {
            showInfo(Messages.usingRuleset(rulesetTitle(view)));
          }
        } else {
          code = (await backendService.fetchRememberedRuleset())?.code;
        }
      } catch (err) {
        log.error(err);
        const status = (err as { status?: number })?.status;
        showError(status === 404 && link.ruleset ? Messages.unknownRuleset(link.ruleset) : err);
        return;
      }
      await create(code);
    },
    [backendService, create, navigate, openLogin, queryClient, showError, showInfo, showWarning],
  );

  useEffect(() => {
    const take = () => {
      window.electron.common
        .takeBrawlLinks()
        .then(async (links) => {
          for (const link of links) {
            await handle(link);
          }
        })
        .catch(log.error);
    };
    take();
    return window.electron.common.onBrawlLink(take);
  }, [handle]);
};
