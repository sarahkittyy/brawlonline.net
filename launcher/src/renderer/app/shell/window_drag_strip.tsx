import styles from "./window_drag_strip.module.css";

// Optional: component tests render the shell without the preload.
const isMac = window.electron?.bootstrap.isMac === true;

/**
 * macOS: the window has no title bar (titleBarStyle hiddenInset), so the empty space along the top of the page and of
 * the players rail drags it (GitHub #56), as the sidebar's does. The strip is the last thing in its box: Electron takes
 * the page's drag and no-drag areas in document order, later ones over earlier ones, so it drags over whatever it
 * covers. Its height is the free space over the first buttons there, so none of them loses a click. Other systems
 * keep their own title bar: nothing.
 */
export const WindowDragStrip = ({ height }: { height: number }) =>
  isMac ? <div className={styles.strip} style={{ height }} aria-hidden={true} data-testid="window-drag-strip" /> : null;
