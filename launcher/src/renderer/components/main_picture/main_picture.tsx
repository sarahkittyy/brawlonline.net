import { clsx } from "clsx";

import styles from "./main_picture.module.css";

// Fox's stock icon's silhouette (traced from its alpha, 32 px scaled to 256 x 256), drawn dotted like a placing
// player's rank emblem (rank_badge.tsx).
const FOX_OUTLINE =
  "M162 15 L153 17 L151 21 L152 32 L150 37 L141 46 L136 48 L116 47 L98 31 L85 31 L81 33 L73 41 L70 53 L57 65 " +
  "L55 69 L56 104 L54 109 L48 115 L46 125 L40 131 L38 141 L32 147 L30 157 L23 165 L23 186 L30 194 L33 206 L57 230 " +
  "L67 232 L77 240 L96 239 L101 241 L109 248 L186 248 L194 241 L206 238 L230 214 L230 209 L223 200 L224 149 " +
  "L217 141 L215 136 L216 109 L214 105 L192 83 L192 53 L185 45 L182 33 Z";

// The profile's edit pencil (MUI's EditOutlined, 24 x 24), 90 units wide in the middle of Fox's head.
const PENCIL =
  "m14.06 9.02.92.92L5.92 19H5v-.92zM17.66 3c-.25 0-.51.1-.7.29l-1.83 1.83 3.75 3.75 1.83-1.83c.39-.39.39-1.02 " +
  "0-1.41l-2.34-2.34c-.2-.2-.45-.29-.71-.29m-3.6 3.19L3 17.25V21h3.75L17.81 9.94z";

/** Put on an ancestor (the header's account button): hovering it shows the pencil in the empty picture. */
export const MAIN_PICTURE_HOVER = styles.hoverHost;

/**
 * A player's picture (docs/design/profiles.md §8): their primary main's stock icon (`url`), or with none (`null`: no
 * main, Random, no disc set up, offline) Fox's stock icon's outline, dotted, with the profile's pencil in it while
 * a `MAIN_PICTURE_HOVER` ancestor is hovered (`pencil`). `undefined` (the profile still loading) keeps the space empty.
 */
export const MainPicture = ({
  url,
  size,
  pencil = false,
  error = false,
}: {
  url: string | null | undefined;
  size: number;
  pencil?: boolean;
  /** The outline red (online play needs setting up). */
  error?: boolean;
}) => {
  if (url) {
    return (
      <img
        className={styles.stock}
        src={url}
        alt=""
        width={size}
        height={size}
        draggable={false}
        data-testid="main-picture"
      />
    );
  }
  if (url === undefined) {
    return <span className={styles.empty} style={{ width: size, height: size }} />;
  }
  return (
    <svg
      className={clsx(styles.none, size < 36 && styles.tiny, error && styles.error)}
      viewBox="0 0 256 256"
      width={size}
      height={size}
      aria-hidden={true}
      data-testid="main-picture-none"
    >
      <path className={styles.outline} d={FOX_OUTLINE} />
      {pencil && <path className={styles.pencil} d={PENCIL} transform="translate(81 92) scale(3.75)" />}
    </svg>
  );
};
