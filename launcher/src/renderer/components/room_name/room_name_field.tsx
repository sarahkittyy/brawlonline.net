import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import React, { useRef, useState } from "react";

import { RoomNameMessages as Messages } from "./room_name.messages";
import styles from "./room_name.module.css";

/** The server's limit (common::rooms::MAX_ROOM_NAME_LEN). */
export const MAX_ROOM_NAME_LEN = 32;

export type RoomNameFieldProps = {
  /** The room's name; null: none, `fallback` is shown in grey. */
  name: string | null;
  /** "alice's room". */
  fallback: string;
  /** The host: the pencil (and a click on the name) opens the box. */
  canEdit: boolean;
  /** Stores the typed name (empty: none); throws when it was refused (the box stays open). */
  onSave: (name: string) => Promise<void>;
  /** The name's text style (the list row's or the panel title's). */
  className?: string;
  /** Stories and screenshots: open with this text. */
  initialEditText?: string;
};

/**
 * A room's name with the host's pencil (docs/design/rooms.md §1.4). Editing is in place: Enter or a click elsewhere
 * saves, Esc goes back. Clicks and keys stay inside, so a list row around it doesn't take them.
 */
export const RoomNameField = ({ name, fallback, canEdit, onSave, className, initialEditText }: RoomNameFieldProps) => {
  const [text, setText] = useState<string | null>(initialEditText ?? null);
  const [saving, setSaving] = useState(false);
  // Esc (or a save under way) closes the box without the blur saving again.
  const closing = useRef(false);
  const editing = text !== null;

  const open = () => {
    if (canEdit && !editing) {
      closing.current = false;
      setText(name ?? "");
    }
  };
  const save = async () => {
    if (text === null || closing.current) {
      return;
    }
    const typed = text.trim().replace(/\s+/g, " ");
    if (typed === (name ?? "")) {
      closing.current = true;
      setText(null);
      return;
    }
    closing.current = true;
    setSaving(true);
    try {
      await onSave(typed);
      setText(null);
    } catch {
      // Refused (the toast says why): the box stays open with the text.
      closing.current = false;
    } finally {
      setSaving(false);
    }
  };
  const stop = (e: React.SyntheticEvent) => e.stopPropagation();

  if (editing) {
    return (
      <span className={styles.edit} onClick={stop} onMouseDown={stop}>
        <input
          className={clsx(styles.input, className)}
          value={text}
          maxLength={MAX_ROOM_NAME_LEN}
          placeholder={fallback}
          aria-label={Messages.roomName()}
          disabled={saving}
          autoFocus={true}
          onFocus={(e) => e.currentTarget.select()}
          onChange={(e) => setText(e.target.value)}
          onBlur={() => void save()}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === "Enter") {
              e.preventDefault();
              void save();
            } else if (e.key === "Escape") {
              e.preventDefault();
              closing.current = true;
              setText(null);
            }
          }}
          data-testid="room-name-input"
        />
        {saving ? (
          <CircularProgress color="inherit" size={12} />
        ) : (
          <span className={styles.count}>{Messages.count(text.length, MAX_ROOM_NAME_LEN)}</span>
        )}
      </span>
    );
  }

  return (
    <span className={styles.display}>
      <span
        className={clsx(styles.text, className, !name && styles.fallback, canEdit && styles.editable)}
        // The whole name where it is cut with "...".
        title={name ?? undefined}
        role={canEdit ? "button" : undefined}
        tabIndex={canEdit ? 0 : undefined}
        onClick={
          canEdit
            ? (e) => {
                e.stopPropagation();
                open();
              }
            : undefined
        }
        onKeyDown={
          canEdit
            ? (e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  e.stopPropagation();
                  open();
                }
              }
            : undefined
        }
        data-testid="room-name"
      >
        {name ?? fallback}
      </span>
      {canEdit && (
        <Tooltip title={Messages.renameTooltip()} placement="top" enterDelay={300} disableInteractive={true}>
          <IconButton
            size="small"
            aria-label={Messages.rename()}
            sx={{
              flexShrink: 0,
              p: "3px",
              color: "var(--text-dim)",
              border: "1px solid rgb(255 255 255 / 14%)",
              borderRadius: "5px",
              "&:hover, &:focus-visible": { color: "var(--text-primary)", borderColor: "rgb(255 255 255 / 30%)" },
            }}
            onClick={(e) => {
              e.stopPropagation();
              open();
            }}
            data-testid="room-name-pencil"
          >
            <EditOutlinedIcon style={{ fontSize: 14 }} />
          </IconButton>
        </Tooltip>
      )}
    </span>
  );
};
