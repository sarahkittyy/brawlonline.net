import { clsx } from "clsx";
import React, { useState } from "react";

import { InviteButton, PlayerName } from "@/components/social/invite_button";
import type { PlayerMenuTarget } from "@/components/social/player_menu";
import { PlayerMenu } from "@/components/social/player_menu";
import { SocialMessages as Messages } from "@/components/social/social.messages";
import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";

import { ContentBlock } from "../content_block/content_block";
import styles from "./online_players.module.css";

/**
 * Everyone online in the launcher (docs/social-protocol.md), under the rating: a name opens a chat
 * tab, the envelope invites, a right click opens the player's menu.
 */
export const OnlinePlayers = React.memo(function OnlinePlayers() {
  const user = useAccount((s) => s.user);
  const hasPlayKey = useAccount((s) => s.userData?.playKey != null);
  const status = useSocial((s) => s.state.status);
  const message = useSocial((s) => s.state.message);
  const online = useSocial((s) => s.state.online);
  const ignored = useSocial((s) => s.state.ignored);
  const openTab = useSocial((s) => s.openTab);
  const [menu, setMenu] = useState<PlayerMenuTarget | null>(null);

  const count =
    status === "online" ? <span className={styles.count}>{Messages.playersOnline(online.length)}</span> : undefined;

  return (
    <div className={styles.block}>
      <ContentBlock fill={true} title={Messages.online()} endIcon={count} content={renderBody()} />
      <PlayerMenu target={menu} onClose={() => setMenu(null)} />
    </div>
  );

  function renderBody() {
    if (!user) {
      return <p className={styles.notice}>{Messages.loggedOut()}</p>;
    }
    if (!hasPlayKey) {
      return <p className={styles.notice}>{Messages.setUpOnline()}</p>;
    }
    if (status !== "online") {
      return <p className={styles.notice}>{status === "off" && message ? message : Messages.connecting()}</p>;
    }
    if (online.length === 0) {
      return <p className={styles.notice}>{Messages.nobodyOnline()}</p>;
    }
    return (
      <ul className={styles.list} data-testid="online-players">
        {online.map((player) => (
          <li key={player.uid}>
            <div
              role="button"
              tabIndex={0}
              className={clsx(styles.row, ignored.includes(player.uid) && styles.ignored)}
              onClick={() => openTab(player.uid, true)}
              onKeyDown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  openTab(player.uid, true);
                }
              }}
              onContextMenu={(e) => {
                e.preventDefault();
                setMenu({ player, x: e.clientX, y: e.clientY });
              }}
            >
              <PlayerName player={player} />
              <InviteButton player={player} />
            </div>
          </li>
        ))}
      </ul>
    );
  }
});
