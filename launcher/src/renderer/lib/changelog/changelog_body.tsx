// A changelog's text in the popup, with the players it mentions (the agent board's `@`, accounts'
// mentions.rs) as the launcher shows names everywhere: their main, flag and badges, the name opening
// their profile, the hover card and the right-click menu. The server's HTML marks each one with a span
// (changelogHtml keeps it as `data-mention`); their name goes inside it, so it flows with the text.
import type { ChangelogMention } from "@accounts/types";
import React from "react";
import { createPortal } from "react-dom";

import { CharacterIcon, useStockIcon } from "@/components/profile/character_icon";
import { UserName } from "@/components/user_name/user_name";

import styles from "./changelog_body.module.css";

/** One mentioned player's name. */
export const ChangelogMentionName = ({ player }: { player: ChangelogMention }) => {
  const stock = useStockIcon(player.main ?? "");
  // A stock icon only when there is one (no disc: none, rather than a tag with the character's name in the text).
  const main = player.mainIcon != null || player.main === "random" || (player.main != null && stock != null);
  return (
    <span className={styles.mention} data-testid="changelog-mention">
      {main && (
        <span className={styles.main}>
          <CharacterIcon
            character={player.main ?? ""}
            icon={player.mainIcon}
            iconLabel={player.mainIconLabel}
            size={18}
          />
        </span>
      )}
      <UserName
        user={player}
        showCode={false}
        showFlag={true}
        badgePlace="profile"
        profileLink={true}
        classes={{ name: styles.name }}
      />
    </span>
  );
};

/**
 * `html` (cleaned by changelogHtml), each `[data-mention]` span of a player in `mentions` holding their name; one not
 * in it keeps its text (`@name`). Styled by the popup (`styled(ChangelogBody)`).
 */
export const ChangelogBody = ({
  html,
  mentions,
  className,
}: {
  html: string;
  mentions: Readonly<Record<string, ChangelogMention>>;
  className?: string;
}) => {
  const ref = React.useRef<HTMLDivElement>(null);
  const [slots, setSlots] = React.useState<{ el: HTMLElement; player: ChangelogMention }[]>([]);
  React.useLayoutEffect(() => {
    const box = ref.current;
    if (!box) {
      return;
    }
    box.innerHTML = html;
    const found: { el: HTMLElement; player: ChangelogMention }[] = [];
    for (const el of Array.from(box.querySelectorAll<HTMLElement>("span[data-mention]"))) {
      const player = mentions[el.dataset.mention ?? ""];
      if (player) {
        el.textContent = "";
        found.push({ el, player });
      }
    }
    setSlots(found);
  }, [html, mentions]);
  return (
    <>
      <div ref={ref} className={className} data-testid="changelog-body" />
      {slots.map(({ el, player }, i) => createPortal(<ChangelogMentionName player={player} />, el, `${i}`))}
    </>
  );
};
