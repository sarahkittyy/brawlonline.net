import CodeRoundedIcon from "@mui/icons-material/CodeRounded";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";

import { DevBadgeMessages as Messages } from "./dev_badge.messages";
import styles from "./dev_badge.module.css";

/**
 * A developer's mark beside their name (an admin account's profile has `dev`): Other Games' Codes `</>` in the
 * codes' purple, "Dev" on hover. Shown to everyone, in the profile dialog and the name's hover card (the website's
 * profile page has the same one).
 */
export const DevBadge = ({ size = 18, className }: { size?: number; className?: string }) => (
  <Tooltip title={Messages.dev()} placement="top" enterDelay={200} disableInteractive={true}>
    <span className={clsx(styles.dev, className)} aria-label={Messages.dev()} role="img" data-testid="dev-badge">
      <CodeRoundedIcon style={{ fontSize: size }} />
    </span>
  </Tooltip>
);
