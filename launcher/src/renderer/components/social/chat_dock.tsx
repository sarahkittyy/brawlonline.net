import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import Tooltip from "@mui/material/Tooltip";
import type { Conversation, SocialPlayer } from "@social/types";
import { clsx } from "clsx";
import React, { useEffect, useLayoutEffect, useRef, useState } from "react";

import { useToasts } from "@/lib/hooks/use_toasts";
import { findPlayer, useSocial } from "@/lib/social/use_social";

import { InviteButton, PlayerName } from "./invite_button";
import type { PlayerMenuTarget } from "./player_menu";
import { PlayerMenu } from "./player_menu";
import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

const WINDOW_WIDTH = 300;
const EDGE = 8;
/** Longest message, in code points (the cleaner's limit; the bytes limit is checked when sent). */
const MAX_INPUT = 100;

/**
 * The bar along the bottom of the window: one tab per conversation, from the left, shrinking to
 * fit. A tab's header opens and closes its chat window, which draws over the page above it.
 */
export const ChatDock = React.memo(function ChatDock() {
  const tabs = useSocial((s) => s.tabs);
  const state = useSocial((s) => s.state);
  const dockRef = useRef<HTMLDivElement>(null);
  const slotRefs = useRef(new Map<string, HTMLDivElement>());
  const [lefts, setLefts] = useState<Record<string, number>>({});
  const [menu, setMenu] = useState<PlayerMenuTarget | null>(null);

  // A window starts at its tab's left edge, kept inside the launcher's window.
  useLayoutEffect(() => {
    const measure = () => {
      const width = dockRef.current?.clientWidth ?? 0;
      const next: Record<string, number> = {};
      for (const [uid, el] of slotRefs.current) {
        next[uid] = Math.max(EDGE, Math.min(el.offsetLeft, width - WINDOW_WIDTH - EDGE));
      }
      setLefts((prev) => (JSON.stringify(prev) === JSON.stringify(next) ? prev : next));
    };
    measure();
    window.addEventListener("resize", measure);
    return () => window.removeEventListener("resize", measure);
  }, [tabs]);

  return (
    <div className={styles.dock} ref={dockRef} data-testid="chat-dock">
      {tabs.map((tab) => {
        const player = findPlayer(state, tab.uid);
        if (!tab.open || !player) {
          return null;
        }
        return (
          <ChatWindow
            key={tab.uid}
            player={player}
            conversation={state.conversations[tab.uid]}
            online={state.online.some((p) => p.uid === tab.uid)}
            ignored={state.ignored.includes(tab.uid)}
            left={lefts[tab.uid] ?? EDGE}
          />
        );
      })}
      <div className={styles.tabs}>
        {tabs.map((tab) => {
          const player = findPlayer(state, tab.uid);
          if (!player) {
            return null;
          }
          return (
            <TabHeader
              key={tab.uid}
              ref={(el) => {
                if (el) {
                  slotRefs.current.set(tab.uid, el);
                } else {
                  slotRefs.current.delete(tab.uid);
                }
              }}
              player={player}
              open={tab.open}
              online={state.online.some((p) => p.uid === tab.uid)}
              unread={(state.conversations[tab.uid]?.unread ?? 0) > 0 && !state.ignored.includes(tab.uid)}
              onMenu={setMenu}
            />
          );
        })}
      </div>
      <PlayerMenu target={menu} onClose={() => setMenu(null)} showMessage={false} />
    </div>
  );
});

const TabHeader = React.forwardRef<
  HTMLDivElement,
  {
    player: SocialPlayer;
    open: boolean;
    online: boolean;
    unread: boolean;
    onMenu: (t: PlayerMenuTarget) => void;
  }
>(function TabHeader({ player, open, online, unread, onMenu }, ref) {
  const toggleTab = useSocial((s) => s.toggleTab);
  const closeTab = useSocial((s) => s.closeTab);
  return (
    <div
      ref={ref}
      role="button"
      tabIndex={0}
      className={clsx(styles.tab, open && styles.open, unread && !open && styles.unread)}
      data-testid={`chat-tab-${player.uid}`}
      onClick={() => toggleTab(player.uid)}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          toggleTab(player.uid);
        }
      }}
      onContextMenu={(e) => {
        e.preventDefault();
        onMenu({ player, x: e.clientX, y: e.clientY });
      }}
    >
      <span className={online ? styles.onlineDot : styles.offlineDot} />
      <span className={styles.tabName}>
        <PlayerName player={player} />
      </span>
      <InviteButton player={player} />
      <button
        type="button"
        className={styles.iconButton}
        aria-label={Messages.closeChat()}
        onClick={(e) => {
          e.stopPropagation();
          closeTab(player.uid);
        }}
      >
        <CloseRoundedIcon style={{ fontSize: 15 }} />
      </button>
    </div>
  );
});

/** Deletes the word before the caret (Ctrl+W, as in a terminal). */
export function deleteWordBefore(text: string, caret: number): { text: string; caret: number } {
  let i = caret;
  while (i > 0 && text[i - 1] === " ") {
    i--;
  }
  while (i > 0 && text[i - 1] !== " ") {
    i--;
  }
  return { text: text.slice(0, i) + text.slice(caret), caret: i };
}

const ChatWindow = ({
  player,
  conversation,
  online,
  ignored,
  left,
}: {
  player: SocialPlayer;
  conversation?: Conversation;
  online: boolean;
  ignored: boolean;
  left: number;
}) => {
  const { showError } = useToasts();
  const [draft, setDraft] = useState("");
  const [sending, setSending] = useState(false);
  const [revealed, setRevealed] = useState<Set<string>>(new Set());
  const inputRef = useRef<HTMLInputElement>(null);
  const linesRef = useRef<HTMLDivElement>(null);
  const lines = conversation?.lines ?? [];
  const unread = conversation?.unread ?? 0;

  useEffect(() => {
    inputRef.current?.focus();
  }, []);

  // Newest at the bottom, scrolled to.
  useLayoutEffect(() => {
    const el = linesRef.current;
    if (el) {
      el.scrollTop = el.scrollHeight;
    }
  }, [lines.length]);

  // Read while open and the launcher has focus.
  useEffect(() => {
    const read = () => {
      if (unread > 0 && document.hasFocus()) {
        void window.electron.social.markRead(player.uid);
      }
    };
    read();
    window.addEventListener("focus", read);
    return () => window.removeEventListener("focus", read);
  }, [player.uid, unread]);

  const send = async () => {
    const text = draft.trim();
    if (!text || sending) {
      return;
    }
    setSending(true);
    try {
      const result = await window.electron.social.sendMessage(player.uid, text);
      if (result.ok) {
        setDraft("");
      } else {
        showError(result.message);
      }
    } finally {
      setSending(false);
      // Stays in the box for the next message.
      requestAnimationFrame(() => inputRef.current?.focus());
    }
  };

  return (
    <div className={styles.window} style={{ left }} data-testid={`chat-window-${player.uid}`}>
      <div className={styles.windowTitle}>
        <PlayerName player={player} />
        <Tooltip title={Messages.e2e()} placement="top" disableInteractive={true}>
          <span className={styles.lock}>
            <LockOutlinedIcon style={{ fontSize: 14 }} />
          </span>
        </Tooltip>
      </div>
      <div className={styles.lines} ref={linesRef}>
        {lines.map((line) => {
          if (!line.own && ignored && !revealed.has(line.id)) {
            return (
              <Tooltip key={line.id} title={Messages.showMessage()} placement="right" disableInteractive={true}>
                <button
                  type="button"
                  className={styles.spoiler}
                  onClick={() => setRevealed((r) => new Set(r).add(line.id))}
                >
                  {line.text}
                </button>
              </Tooltip>
            );
          }
          return (
            <div key={line.id} className={clsx(styles.line, line.own ? styles.mine : styles.theirs)}>
              {line.text}
            </div>
          );
        })}
        {ignored && <div className={styles.notice}>{Messages.ignoredNotice()}</div>}
        {!online && <div className={styles.notice}>{Messages.offlineNotice()}</div>}
      </div>
      <div className={styles.inputRow}>
        <input
          ref={inputRef}
          className={styles.input}
          value={draft}
          maxLength={MAX_INPUT * 2}
          placeholder={Messages.typeMessage()}
          disabled={!online}
          spellCheck={true}
          onChange={(e) => setDraft(Array.from(e.target.value).slice(0, MAX_INPUT).join(""))}
          onKeyDown={(e) => {
            if (e.key === "Enter" && !e.nativeEvent.isComposing) {
              e.preventDefault();
              void send();
            } else if (e.key === "Escape") {
              e.preventDefault();
              useSocial.getState().toggleTab(player.uid);
            } else if ((e.key === "w" || e.key === "W") && e.ctrlKey && !e.altKey && !e.shiftKey) {
              e.preventDefault();
              const el = e.currentTarget;
              const caret = el.selectionStart ?? draft.length;
              const next = deleteWordBefore(draft, caret);
              setDraft(next.text);
              requestAnimationFrame(() => el.setSelectionRange(next.caret, next.caret));
            }
          }}
        />
      </div>
    </div>
  );
};
