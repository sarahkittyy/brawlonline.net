import { clsx } from "clsx";

import styles from "./content_block.module.css";

export const ContentBlock = ({
  title,
  content,
  onClick,
  fill,
  endIcon,
  titleAction,
  toolbar,
  overflowY = "auto",
}: {
  fill?: boolean;
  /** Text, or a room's name field (Other Games' room panel). */
  title?: React.ReactNode;
  /** Shown right after the title (a button). */
  titleAction?: React.ReactNode;
  /** A row under the header that doesn't scroll with the content (buttons, boxes). */
  toolbar?: React.ReactNode;
  content: React.ReactNode;
  onClick?: () => void;
  endIcon?: React.ReactNode;
  overflowY?: "auto" | "hidden";
}) => {
  return (
    <div className={clsx(styles.outer, { [styles.fill]: fill })}>
      {title && (
        <div className={clsx(styles.header, onClick && styles.active)} onClick={onClick}>
          <div className={styles.titleRow}>
            <h3 className={clsx(styles.title, typeof title !== "string" && styles.shrinks)}>{title}</h3>
            {titleAction}
          </div>
          {endIcon}
        </div>
      )}
      {toolbar && <div className={styles.toolbar}>{toolbar}</div>}
      <div className={styles.content} style={{ overflowY }}>
        {content}
      </div>
    </div>
  );
};
