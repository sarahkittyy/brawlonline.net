import FavoriteBorderIcon from "@mui/icons-material/FavoriteBorder";
import React from "react";

import { ExternalLink as A } from "@/components/external_link";

import { BRAWLBACK_CONTRIBUTORS, BRAWLBACK_URL, BUILT_ON } from "./credits";
import { CreditsPageMessages as Messages } from "./credits_page.messages";
import styles from "./credits_page.module.css";

export const CreditsPage = React.memo(() => {
  return (
    <div className={styles.container}>
      <h2 className={styles.heading}>
        <FavoriteBorderIcon className={styles.headingIcon} />
        {Messages.credits()}
      </h2>
      <div>{Messages.brawlbackIntro()}</div>
      <div className={styles.names}>
        {BRAWLBACK_CONTRIBUTORS.map((c) => (
          <A key={c.name} className={styles.link} href={c.url}>
            {c.name}
          </A>
        ))}
      </div>
      <div className={styles.names}>
        <A className={styles.link} href={BRAWLBACK_URL}>
          github.com/Brawlback-Team
        </A>
      </div>
      <h3 className={styles.subheading}>{Messages.alsoBuiltOn()}</h3>
      <ul className={styles.list}>
        {BUILT_ON.map((c) => (
          <li key={c.name}>
            <A className={styles.link} href={c.url}>
              {c.name}
            </A>
            {c.note && <span className={styles.note}> ({c.note})</span>}
          </li>
        ))}
      </ul>
    </div>
  );
});
