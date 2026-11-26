/**
 * Settings > Social > Quick Chat (common/quick_chat.ts, docs/design/quick-chat.md): the 16 D-pad
 * messages, edited where the game shows them. The D-pad in the middle picks the group (its arm, the
 * first press); the four messages sit around it on the side of the second press, as the game's menu
 * draws them over the CSS (core/ui QuickChatUI.cpp). Each arm's four dots are its messages, lit
 * when set. Arrow keys on the D-pad pick the group as the controller's D-pad would.
 */
import type { QuickChatDirection } from "@common/quick_chat";
import {
  limitQuickChatMessage,
  normalizeQuickChat,
  QUICK_CHAT_DIRECTIONS,
  QUICK_CHAT_MAX_CHARACTERS,
  quickChatIndex,
  quickChatLength,
} from "@common/quick_chat";
import { css, keyframes } from "@emotion/react";
import React from "react";

import { useQuickChat } from "@/lib/hooks/use_settings";

const ARROWS: Record<QuickChatDirection, string> = { up: "↑", right: "→", down: "↓", left: "←" };
const KEYS: Record<string, QuickChatDirection> = {
  ArrowUp: "up",
  ArrowRight: "right",
  ArrowDown: "down",
  ArrowLeft: "left",
};
// The D-pad's cells (3 x 3) and each arm's rounded outer corners.
const ARM_CELL: Record<QuickChatDirection, string> = {
  up: "1 / 2 / 2 / 3",
  right: "2 / 3 / 3 / 4",
  down: "3 / 2 / 4 / 3",
  left: "2 / 1 / 3 / 2",
};
const ARM_RADIUS: Record<QuickChatDirection, string> = {
  up: "9px 9px 0 0",
  right: "0 9px 9px 0",
  down: "0 0 9px 9px",
  left: "9px 0 0 9px",
};
// Where a dot or notch sits in its arm: toward the tip.
const ROTATE: Record<QuickChatDirection, number> = { up: 0, right: 90, down: 180, left: 270 };
const SAVE_AFTER_MS = 500;

const appear = keyframes`
  from { opacity: 0; transform: translateY(2px); }
  to { opacity: 1; transform: none; }
`;

/** A message's field, as tall as its text, aligned as the game draws it on that side. */
const MessageField = ({
  value,
  align,
  label,
  disabled,
  onChange,
  onDone,
}: {
  value: string;
  align: "left" | "center" | "right";
  label: string;
  disabled: boolean;
  onChange: (value: string) => void;
  onDone: () => void;
}) => {
  const ref = React.useRef<HTMLTextAreaElement>(null);
  const [focused, setFocused] = React.useState(false);
  React.useLayoutEffect(() => {
    const el = ref.current;
    if (el) {
      el.style.height = "auto";
      el.style.height = `${el.scrollHeight}px`;
    }
  }, [value, focused]);
  const length = quickChatLength(value);
  const showCount = focused || length > QUICK_CHAT_MAX_CHARACTERS * 0.8;
  return (
    <div
      css={css`
        position: relative;
        width: 100%;
      `}
    >
      <textarea
        ref={ref}
        rows={1}
        value={value}
        aria-label={label}
        disabled={disabled}
        spellCheck={false}
        placeholder="–"
        onFocus={() => setFocused(true)}
        onBlur={() => {
          setFocused(false);
          onDone();
        }}
        onChange={(e) => onChange(limitQuickChatMessage(e.target.value))}
        onKeyDown={(e) => {
          // One line each, as chat sends it: Enter is done.
          if (e.key === "Enter") {
            e.preventDefault();
            e.currentTarget.blur();
          }
        }}
        css={css`
          display: block;
          box-sizing: border-box;
          width: 100%;
          resize: none;
          overflow: hidden;
          font: inherit;
          font-size: 14px;
          line-height: 20px;
          text-align: ${align};
          color: var(--text-primary);
          background: rgba(255, 255, 255, 0.035);
          border: 1px solid var(--line-soft);
          border-radius: 10px;
          padding: 9px 12px ${showCount ? 22 : 9}px;
          outline: none;
          transition: border-color 120ms ease, box-shadow 120ms ease, background 120ms ease;
          &::placeholder {
            color: var(--text-very-dim);
          }
          &:hover:not(:disabled) {
            background: rgba(255, 255, 255, 0.055);
          }
          &:focus {
            border-color: var(--accent-primary);
            box-shadow: 0 0 0 3px color-mix(in oklab, var(--accent-primary) 22%, transparent);
            background: rgba(255, 255, 255, 0.06);
          }
        `}
      />
      {showCount && (
        <span
          aria-hidden={true}
          css={css`
            position: absolute;
            bottom: 5px;
            ${align === "right" ? "left" : "right"}: 10px;
            font-size: 11px;
            font-variant-numeric: tabular-nums;
            pointer-events: none;
            color: ${length >= QUICK_CHAT_MAX_CHARACTERS ? "var(--accent-primary-bright)" : "var(--text-very-dim)"};
          `}
        >
          {length}/{QUICK_CHAT_MAX_CHARACTERS}
        </span>
      )}
    </div>
  );
};

/** The four dots of an arm's group, in a little cross: one per message, lit when it is set. */
const GroupDots = ({ filled, lit }: { filled: boolean[]; lit: boolean }) => (
  <span
    aria-hidden={true}
    css={css`
      display: grid;
      grid-template: repeat(3, 4px) / repeat(3, 4px);
      gap: 1px;
    `}
  >
    {QUICK_CHAT_DIRECTIONS.map((d, i) => (
      <span
        key={d}
        css={css`
          grid-area: ${["1 / 2", "2 / 3", "3 / 2", "2 / 1"][i]};
          border-radius: 50%;
          background: ${filled[i]
            ? lit
              ? "var(--surface-0)"
              : "var(--text-secondary)"
            : lit
            ? "color-mix(in oklab, var(--surface-0) 25%, transparent)"
            : "rgba(255, 255, 255, 0.12)"};
        `}
      />
    ))}
  </span>
);

/** The D-pad: an arm per group, the open one lit in the theme's colour. */
const DPad = ({
  group,
  messages,
  disabled,
  onPick,
}: {
  group: QuickChatDirection;
  messages: string[];
  disabled: boolean;
  onPick: (group: QuickChatDirection) => void;
}) => {
  const arms = React.useRef<Partial<Record<QuickChatDirection, HTMLButtonElement | null>>>({});
  return (
    <div
      role="radiogroup"
      onKeyDown={(e) => {
        const d = KEYS[e.key];
        if (d) {
          e.preventDefault();
          onPick(d);
          arms.current[d]?.focus();
        }
      }}
      css={css`
        display: grid;
        grid-template: repeat(3, 40px) / repeat(3, 40px);
        filter: drop-shadow(0 2px 6px rgba(0, 0, 0, 0.35));
      `}
    >
      <span
        aria-hidden={true}
        css={css`
          grid-area: 2 / 2 / 3 / 3;
          background: #3c3f4a;
          display: flex;
          align-items: center;
          justify-content: center;
          &::after {
            content: "";
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: rgba(0, 0, 0, 0.22);
          }
        `}
      />
      {QUICK_CHAT_DIRECTIONS.map((d) => {
        const lit = d === group;
        const filled = QUICK_CHAT_DIRECTIONS.map((m) => messages[quickChatIndex(d, m)].trim() !== "");
        return (
          <button
            key={d}
            ref={(el) => {
              arms.current[d] = el;
            }}
            type="button"
            role="radio"
            aria-checked={lit}
            aria-label={ARROWS[d]}
            tabIndex={lit ? 0 : -1}
            disabled={disabled}
            onClick={() => onPick(d)}
            css={css`
              grid-area: ${ARM_CELL[d]};
              border: none;
              padding: 0;
              margin: 0;
              cursor: pointer;
              border-radius: ${ARM_RADIUS[d]};
              background: ${lit ? "var(--accent-primary)" : "#3c3f4a"};
              display: flex;
              align-items: center;
              justify-content: center;
              position: relative;
              transition: background 120ms ease;
              outline: none;
              &:hover:not(:disabled) {
                background: ${lit ? "var(--accent-primary-bright)" : "#4a4e5b"};
              }
              &:focus-visible {
                box-shadow: inset 0 0 0 2px var(--text-primary);
              }
              /* The notch at the arm's tip. */
              &::before {
                content: "";
                position: absolute;
                left: 50%;
                top: 50%;
                width: 0;
                height: 0;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-bottom: 6px solid ${lit ? "var(--surface-0)" : "rgba(255, 255, 255, 0.45)"};
                transform: translate(-50%, -50%) rotate(${ROTATE[d]}deg) translateY(-12px);
              }
            `}
          >
            <span
              css={css`
                transform: rotate(${ROTATE[d]}deg) translateY(4px) rotate(-${ROTATE[d]}deg);
              `}
            >
              <GroupDots filled={filled} lit={lit} />
            </span>
          </button>
        );
      })}
    </div>
  );
};

export const QuickChatSetting = ({ disabled }: { disabled: boolean }) => {
  const [stored, setStored] = useQuickChat();
  const [group, setGroup] = React.useState<QuickChatDirection>("up");
  // What is being typed, saved a moment after the last key (and on leaving a field).
  const [draft, setDraft] = React.useState<string[] | null>(null);
  const saved = React.useMemo(() => normalizeQuickChat(stored), [stored]);
  const messages = draft ?? saved;
  const timer = React.useRef<number | undefined>(undefined);
  const latest = React.useRef(messages);
  latest.current = messages;

  const save = React.useCallback(() => {
    window.clearTimeout(timer.current);
    timer.current = undefined;
    if (draft) {
      void setStored(draft);
    }
  }, [draft, setStored]);
  const saveRef = React.useRef(save);
  saveRef.current = save;
  // The draft goes once the setting has it (until then it is what the fields show).
  React.useEffect(() => {
    if (draft && timer.current === undefined && draft.every((m, i) => m === saved[i])) {
      setDraft(null);
    }
  }, [draft, saved]);
  // Leaving the page saves what was typed.
  React.useEffect(() => () => saveRef.current(), []);

  const change = (direction: QuickChatDirection, value: string) => {
    const next = [...latest.current];
    next[quickChatIndex(group, direction)] = value;
    setDraft(next);
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(() => saveRef.current(), SAVE_AFTER_MS);
  };

  const field = (direction: QuickChatDirection, align: "left" | "center" | "right") => (
    <MessageField
      value={messages[quickChatIndex(group, direction)]}
      align={align}
      label={`${ARROWS[group]} ${ARROWS[direction]}`}
      disabled={disabled}
      onChange={(v) => change(direction, v)}
      onDone={save}
    />
  );

  return (
    <div
      css={css`
        display: grid;
        grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr);
        grid-template-areas:
          "up up up"
          "left pad right"
          "down down down";
        align-items: center;
        gap: 14px 18px;
        padding: 22px 22px 24px;
        border-radius: 16px;
        background: var(--surface-1);
        border: 1px solid var(--line-soft);
        box-shadow: 0 8px 24px rgba(0, 0, 0, 0.22);
        opacity: ${disabled ? 0.45 : 1};
        transition: opacity 150ms ease;
      `}
    >
      <div
        key={`up-${group}`}
        css={css`
          grid-area: up;
          justify-self: center;
          width: min(100%, 420px);
          animation: ${appear} 140ms ease-out;
        `}
      >
        {field("up", "center")}
      </div>
      <div
        key={`left-${group}`}
        css={css`
          grid-area: left;
          animation: ${appear} 140ms ease-out;
        `}
      >
        {field("left", "right")}
      </div>
      <div
        css={css`
          grid-area: pad;
        `}
      >
        <DPad group={group} messages={messages} disabled={disabled} onPick={setGroup} />
      </div>
      <div
        key={`right-${group}`}
        css={css`
          grid-area: right;
          animation: ${appear} 140ms ease-out;
        `}
      >
        {field("right", "left")}
      </div>
      <div
        key={`down-${group}`}
        css={css`
          grid-area: down;
          justify-self: center;
          width: min(100%, 420px);
          animation: ${appear} 140ms ease-out;
        `}
      >
        {field("down", "center")}
      </div>
    </div>
  );
};
