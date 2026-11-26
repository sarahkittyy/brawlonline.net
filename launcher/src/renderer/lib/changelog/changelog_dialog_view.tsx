// The changelog popup (docs/design/changelog.md): Sarah's own Markdown from the agent board's patch
// notes, every release's, newest first in one tall popup that scrolls inside itself like a feed. Its
// top edge stays put (the popup grows down to the window's bottom margin, then scrolls); the wheel
// scrolls it anywhere over the window, and the next changelogs are asked for only as its end comes
// into sight. The X stays in the popup's top right corner, above the feed, which never runs under
// it. The X (or Esc) closes it; a click beside it doesn't (a click in the launcher as it opens would
// lose it).
// The text's rules are also the board's preview's (agent-board lib/patchnotes, .cl-body): change both.
import styled from "@emotion/styled";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import Dialog from "@mui/material/Dialog";
import DialogContent from "@mui/material/DialogContent";
import IconButton from "@mui/material/IconButton";
import React from "react";

import { AddonsSettingsMessages } from "@/pages/settings/addons_settings/addons_settings.messages";
import { bodyFont, titleFont } from "@/styles/with_font";

import { ChangelogMessages as Messages } from "./changelog.messages";
import type { ChangelogEntry } from "./changelog_feed";

export type ChangelogDialogViewProps = {
  open: boolean;
  /** Newest first. */
  entries: readonly (Pick<ChangelogEntry, "id" | "html"> & Partial<Pick<ChangelogEntry, "emissaryVersion">>)[];
  /** There are older ones still to ask for. */
  hasMore: boolean;
  onLoadMore: () => void;
  onClose: () => void;
};

/** How far below the feed's end may be (px) before the next changelogs are asked for. */
const LOAD_AHEAD = 300;
/** A wheel's line, for a wheel that counts in lines. */
const LINE_PX = 40;

const KEY_SCROLL: Record<string, (page: number) => ScrollToOptions> = {
  ArrowDown: () => ({ top: LINE_PX }),
  ArrowUp: () => ({ top: -LINE_PX }),
  PageDown: (page) => ({ top: page }),
  PageUp: (page) => ({ top: -page }),
  " ": (page) => ({ top: page }),
};

// Pictures and videos still loading: the feed's height isn't known yet.
const stillLoading = (box: HTMLElement) =>
  Array.from(box.querySelectorAll("img")).some((img) => !img.complete) ||
  Array.from(box.querySelectorAll("video")).some((v) => v.readyState < 1 && v.error == null && v.networkState !== 3);

export const ChangelogDialogView = ({ open, entries, hasMore, onLoadMore, onClose }: ChangelogDialogViewProps) => {
  const [scroller, setScroller] = React.useState<HTMLDivElement | null>(null);

  // More only once the feed's end is in sight with everything above it loaded.
  const check = React.useCallback(() => {
    // No height yet (a window not shown): nothing is known to fit.
    if (!scroller || !hasMore || scroller.clientHeight === 0 || stillLoading(scroller)) {
      return;
    }
    if (scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < LOAD_AHEAD) {
      onLoadMore();
    }
  }, [scroller, hasMore, onLoadMore]);

  React.useEffect(() => {
    check();
  }, [check, entries]);

  React.useEffect(() => {
    if (!scroller) {
      return;
    }
    // A picture or video as it loads (load doesn't bubble: listened for on the way down).
    const onLoaded = () => check();
    for (const type of ["load", "error", "loadedmetadata"]) {
      scroller.addEventListener(type, onLoaded, true);
    }
    // A taller window (or one shown at last) may fit more.
    const resized = new ResizeObserver(onLoaded);
    resized.observe(scroller);
    return () => {
      resized.disconnect();
      for (const type of ["load", "error", "loadedmetadata"]) {
        scroller.removeEventListener(type, onLoaded, true);
      }
    };
  }, [scroller, check]);

  // The wheel over the rest of the window (and the X) scrolls the feed too.
  const onWheel = (e: React.WheelEvent) => {
    if (!scroller || scroller.contains(e.target as Node)) {
      return;
    }
    const unit = e.deltaMode === 1 ? LINE_PX : e.deltaMode === 2 ? scroller.clientHeight : 1;
    scroller.scrollBy({ top: e.deltaY * unit });
  };

  // So do the keys, with the focus on the popup rather than in the feed (as it opens).
  const onKeyDown = (e: React.KeyboardEvent) => {
    const by = KEY_SCROLL[e.key];
    if (!scroller || !by || scroller.contains(e.target as Node) || e.target instanceof HTMLButtonElement) {
      return;
    }
    e.preventDefault();
    scroller.scrollBy(by(scroller.clientHeight * 0.85));
  };

  return (
    <Dialog
      open={open}
      onClose={(_e, reason) => reason !== "backdropClick" && onClose()}
      onWheel={onWheel}
      onKeyDown={onKeyDown}
      fullWidth={true}
      scroll="paper"
      // The top edge where a tall popup's always was; it grows downwards only.
      sx={{ "& .MuiDialog-container": { alignItems: "flex-start" } }}
      PaperProps={{ style: { maxWidth: 600 } }}
    >
      <IconButton
        aria-label={Messages.close()}
        onClick={onClose}
        size="small"
        style={{ position: "absolute", right: 14, top: 12, zIndex: 1 }}
        data-testid="changelog-close"
      >
        <CloseRoundedIcon fontSize="small" />
      </IconButton>
      <DialogContent
        ref={setScroller}
        onScroll={check}
        style={{ padding: "20px 24px 24px" }}
        data-testid="changelog-feed"
      >
        {entries.map((e, i) => (
          <React.Fragment key={e.id}>
            {i > 0 && <Divider />}
            {e.emissaryVersion != null && (
              <EmissaryTag data-testid="changelog-emissary">
                {`${AddonsSettingsMessages.playWithEmissaryOn()} ${e.emissaryVersion}`.trim()}
              </EmissaryTag>
            )}
            <Body dangerouslySetInnerHTML={{ __html: e.html }} data-testid="changelog-body" />
          </React.Fragment>
        ))}
      </DialogContent>
    </Dialog>
  );
};

/** Over EmissaryRecomp's patch notes: whose they are, as the board's preview shows it. */
const EmissaryTag = styled.div`
  display: inline-block;
  margin-bottom: 10px;
  padding: 2px 9px;
  border-radius: 999px;
  background-color: rgba(255, 255, 255, 0.1);
  font-family: ${bodyFont};
  font-size: 12px;
  line-height: 18px;
  color: rgba(255, 255, 255, 0.8);
`;

/** Between two releases' changelogs. */
const Divider = styled.hr`
  margin: 28px 0 24px;
  border: 0;
  border-top: 1px solid rgba(255, 255, 255, 0.15);
`;

const Body = styled.div`
  font-family: ${bodyFont};
  font-size: 15px;
  line-height: 1.55;
  padding-right: 24px;
  user-select: text;

  * {
    user-select: text;
  }

  & > :first-child {
    margin-top: 0;
  }

  & > :last-child {
    margin-bottom: 0;
  }

  h2 {
    margin: 22px 0 8px;
    font-family: ${titleFont};
    font-size: 22px;
    font-weight: 700;
    line-height: 1.25;
  }

  h3,
  h4 {
    margin: 18px 0 6px;
    font-family: ${titleFont};
    font-size: 17px;
    font-weight: 700;
  }

  p {
    margin: 0 0 10px;
  }

  ul,
  ol {
    margin: 0 0 10px;
    padding-left: 22px;
  }

  li {
    margin: 4px 0;
  }

  a {
    color: inherit;
    text-decoration: underline;
    word-break: break-word;
  }

  code {
    padding: 1px 4px;
    border-radius: 3px;
    background: rgba(255, 255, 255, 0.08);
    font: 0.9em ui-monospace, "Cascadia Mono", Consolas, monospace;
  }

  pre {
    margin: 0 0 10px;
    padding: 10px 12px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.06);
    overflow: auto;
  }

  pre code {
    padding: 0;
    background: none;
  }

  blockquote {
    margin: 0 0 10px;
    padding: 2px 12px;
    border-left: 3px solid rgba(255, 255, 255, 0.25);
    opacity: 0.85;
  }

  hr {
    margin: 16px 0;
    border: 0;
    border-top: 1px solid rgba(255, 255, 255, 0.15);
  }

  figure {
    margin: 14px 0;
  }

  img,
  video {
    max-width: 100%;
    height: auto;
    border-radius: 4px;
  }

  /* A picture in a line of text (a paragraph, a heading, a list item, a table cell) sits in the line,
     like an icon; only a figure's picture (one alone in its paragraph) and a video stand on their own. */
  img {
    vertical-align: middle;
  }

  figure img,
  video {
    display: block;
    margin: 0 auto;
  }

  figcaption {
    margin-top: 6px;
    font-size: 13px;
    text-align: center;
    opacity: 0.7;
  }

  table {
    margin: 0 0 10px;
    border-collapse: collapse;
  }

  th,
  td {
    padding: 4px 8px;
    border: 1px solid rgba(255, 255, 255, 0.15);
  }
`;
