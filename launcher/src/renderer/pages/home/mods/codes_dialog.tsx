import { fitsRoom, isRunnable, looksLikeActionReplay, parsePastedCode } from "@dolphin/mods/code_rules";
import type { ModBuild, ModBuildCodes, ModCode, ModCodeLists, ModCodeSource, ModOnCode } from "@dolphin/mods/types";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import CodeRoundedIcon from "@mui/icons-material/CodeRounded";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import ExpandMoreRoundedIcon from "@mui/icons-material/ExpandMoreRounded";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import SearchRoundedIcon from "@mui/icons-material/SearchRounded";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import { useEffect, useState } from "react";

import { CodesMessages as Messages } from "./codes.messages";
import styles from "./codes.module.css";

/** The codes' purple on a switch that is on (the theme's switches are grey either way). */
const ON_SWITCH = {
  "& .MuiSwitch-switchBase.Mui-checked": { color: "#c9a6f2" },
  "& .MuiSwitch-switchBase.Mui-checked + .MuiSwitch-track": { backgroundColor: "#b98ce0", opacity: 0.55 },
  "& .MuiSwitch-switchBase.Mui-checked.Mui-disabled": { color: "#a98cc8" },
  "& .MuiSwitch-switchBase.Mui-checked.Mui-disabled + .MuiSwitch-track": { backgroundColor: "#b98ce0", opacity: 0.3 },
};

/** A game whose rooms can run codes: a disc booted as it is (a mod's launcher, P+'s too, puts its own
 * Gecko code handler where Dolphin's goes). */
export const hasRoomCodes = (build: Pick<ModBuild, "builtIn" | "launcherPath">) =>
  !build.builtIn && !build.launcherPath;

/** A tooltip's list of code names: 8, then "+N more". */
export const CodeNames = ({ names }: { names: string[] }) => (
  <div className={styles.tipList}>
    {names.slice(0, 8).map((n, i) => (
      <div key={`${i}-${n}`}>{n}</div>
    ))}
    {names.length > 8 && <div className={styles.tipMore}>{Messages.more(names.length - 8)}</div>}
  </div>
);

/** A game's Codes button: how many are on, their names on hover. */
export const CodesButton = ({
  names,
  disabled,
  onClick,
}: {
  names: string[];
  disabled?: boolean;
  onClick: () => void;
}) => (
  <Tooltip title={names.length ? <CodeNames names={names} /> : Messages.codes()} placement="top">
    <span>
      <Button
        size="small"
        disabled={disabled}
        onClick={onClick}
        className={clsx(styles.codesButton, names.length === 0 && styles.codesNone)}
        startIcon={<CodeRoundedIcon />}
        aria-label={Messages.codes()}
      >
        {names.length ? names.length : Messages.codes()}
      </Button>
    </span>
  </Tooltip>
);

/** A room's codes beside its Disc and SD card chips ("<> 6"), their names on hover. */
export const CodesChip = ({ names, onClick }: { names: string[]; onClick?: () => void }) => (
  <Tooltip title={<CodeNames names={names} />} disableInteractive={true}>
    <span
      role={onClick ? "button" : undefined}
      tabIndex={onClick ? 0 : undefined}
      className={clsx(styles.codesChip, onClick && styles.codesChipClickable)}
      onClick={(e) => {
        if (onClick) {
          e.stopPropagation();
          onClick();
        }
      }}
      onKeyDown={(e) => {
        if (onClick && (e.key === "Enter" || e.key === " ")) {
          e.preventDefault();
          onClick();
        }
      }}
    >
      <CodeRoundedIcon className={styles.chipIcon} />
      {names.length}
    </span>
  </Tooltip>
);

const sameCode = (a: ModCode, b: ModCode) => a.kind === b.kind && a.name === b.name;
const isOn = (codes: ModBuildCodes, source: ModCodeSource, code: ModCode) =>
  codes.on.some((o) => o.source === source && sameCode(o, code));

const CodeRow = ({
  code,
  on,
  locked,
  blocked,
  onToggle,
  onEdit,
  onDelete,
}: {
  code: ModCode;
  on: boolean;
  locked: boolean;
  /** Why it can't be turned on, if it can't. */
  blocked: string | null;
  onToggle: () => void;
  onEdit?: () => void;
  onDelete?: () => void;
}) => {
  const [open, setOpen] = useState(false);
  // An Action Replay code's name keeps its "[author]" (Dolphin's ini names it so): shown apart.
  const arAuthor = code.kind === "ar" && !code.author ? /^(.*?)\s*\[([^\]]+)\]$/.exec(code.name) : null;
  const title = arAuthor ? { name: arAuthor[1], author: arAuthor[2] } : { name: code.name, author: code.author };
  const toggle = (
    <Switch
      size="small"
      checked={on}
      disabled={locked || (!on && blocked !== null)}
      onChange={onToggle}
      sx={ON_SWITCH}
      inputProps={{ "aria-label": code.name }}
    />
  );
  return (
    <div className={clsx(styles.row, open && styles.rowOpen)}>
      <div className={styles.rowHead}>
        {!on && blocked && !locked ? (
          <Tooltip title={blocked} placement="left" disableInteractive={true}>
            <span>{toggle}</span>
          </Tooltip>
        ) : (
          toggle
        )}
        <span className={clsx(styles.codeName, !on && styles.off)} onClick={() => setOpen(!open)}>
          {title.name}
          {title.author && <span className={styles.author}>{title.author}</span>}
          {code.kind === "ar" && <span className={styles.kind}>AR</span>}
        </span>
        {open && onEdit && !locked && (
          <IconButton size="small" aria-label={Messages.edit()} onClick={onEdit}>
            <EditOutlinedIcon fontSize="small" />
          </IconButton>
        )}
        {open && onDelete && !locked && (
          <IconButton size="small" aria-label={Messages.delete()} onClick={onDelete}>
            <DeleteOutlineRoundedIcon fontSize="small" />
          </IconButton>
        )}
        <IconButton size="small" aria-label={Messages.show()} onClick={() => setOpen(!open)}>
          <ExpandMoreRoundedIcon fontSize="small" className={clsx(styles.chevron, open && styles.chevronOpen)} />
        </IconButton>
      </div>
      {open && (
        <div className={styles.rowBody}>
          {code.notes && <div className={styles.notes}>{code.notes}</div>}
          <pre className={styles.lines}>{code.lines.join("\n")}</pre>
        </div>
      )}
    </div>
  );
};

/** The code database's list: fetched when its section first opens. */
export type DatabaseCodes =
  | { kind: "idle" }
  | { kind: "loading" }
  | { kind: "failed"; error: string }
  | { kind: "ready"; codes: ModCode[] };

/**
 * A game's codes (docs/design/room-codes.md): every list with a switch per code, the ones on run
 * in the game's next room. Each change is kept at once (onChange). Locked while the game's room
 * is open.
 */
export const CodesDialog = ({
  open,
  game,
  gameId,
  codes,
  lists,
  database,
  locked,
  onChange,
  onLoadDatabase,
  onClose,
}: {
  open: boolean;
  game: string;
  gameId?: string | null;
  codes: ModBuildCodes;
  /** Null while they load. */
  lists: ModCodeLists | null;
  database: DatabaseCodes;
  locked: boolean;
  onChange: (codes: ModBuildCodes) => void;
  onLoadDatabase: () => void;
  onClose: () => void;
}) => {
  const [filter, setFilter] = useState<"all" | "on">("all");
  const [query, setQuery] = useState("");
  const [databaseOpen, setDatabaseOpen] = useState(false);
  // The code being edited (null: a new one), or none.
  const [editing, setEditing] = useState<{ code: ModCode | null } | null>(null);
  useEffect(() => {
    if (open) {
      // While its room is open, the ones it runs.
      setFilter(locked ? "on" : "all");
      setQuery("");
      setDatabaseOpen(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);

  const q = query.trim().toLowerCase();
  const shown = (code: ModCode, source: ModCodeSource) =>
    (filter === "all" || isOn(codes, source, code)) &&
    (!q || code.name.toLowerCase().includes(q) || (code.author ?? "").toLowerCase().includes(q));
  // A list's codes, and the ones on from it that it no longer has (still run, so still shown).
  const withOn = (source: ModCodeSource, list: ModCode[]) => [
    ...list,
    ...codes.on.filter((o) => o.source === source && !list.some((c) => sameCode(c, o))),
  ];
  const toggle = (source: ModCodeSource, code: ModCode) => {
    if (isOn(codes, source, code)) {
      onChange({ ...codes, on: codes.on.filter((o) => !(o.source === source && sameCode(o, code))) });
    } else {
      const { name, kind, lines, author, notes } = code;
      const added: ModOnCode = {
        name,
        kind,
        lines,
        source,
        ...(author ? { author } : {}),
        ...(notes ? { notes } : {}),
      };
      onChange({ ...codes, on: [...codes.on, added] });
    }
  };
  const blocked = (source: ModCodeSource, code: ModCode): string | null => {
    if (!isRunnable(code)) {
      return Messages.notRunnable();
    }
    return isOn(codes, source, code) || fitsRoom([...codes.on, code]) ? null : Messages.noRoom();
  };
  const saveCode = (code: ModCode) => {
    const old = editing?.code;
    const yours = old ? codes.yours.map((c) => (sameCode(c, old) ? code : c)) : [...codes.yours, code];
    let on = codes.on;
    if (old && isOn(codes, "yours", old)) {
      on = on.map((o) => (o.source === "yours" && sameCode(o, old) ? { ...code, source: "yours" as const } : o));
    } else if (!old && fitsRoom([...on, code])) {
      // A new code is on at once: it was pasted to be played.
      on = [...on, { ...code, source: "yours" as const }];
    }
    onChange({ on, yours });
    setEditing(null);
  };
  const deleteCode = (code: ModCode) => {
    onChange({
      on: codes.on.filter((o) => !(o.source === "yours" && sameCode(o, code))),
      yours: codes.yours.filter((c) => !sameCode(c, code)),
    });
  };

  const sections: { source: ModCodeSource; label: string; hint?: string; list: ModCode[] }[] = [
    { source: "yours", label: Messages.yours(), list: withOn("yours", codes.yours) },
    {
      source: "own",
      label: Messages.own(),
      hint: lists?.ownFiles.join("\n"),
      list: withOn("own", lists?.own ?? []),
    },
    { source: "dolphin", label: Messages.dolphin(), list: withOn("dolphin", lists?.dolphin ?? []) },
  ];
  const databaseList = withOn("database", database.kind === "ready" ? database.codes : []);
  const nothing = lists !== null && sections.every((s) => s.list.length === 0) && database.kind !== "loading";

  const row = (source: ModCodeSource, code: ModCode) => (
    <CodeRow
      key={`${source}-${code.kind}-${code.name}`}
      code={code}
      on={isOn(codes, source, code)}
      locked={locked}
      blocked={blocked(source, code)}
      onToggle={() => toggle(source, code)}
      onEdit={source === "yours" ? () => setEditing({ code }) : undefined}
      onDelete={source === "yours" ? () => deleteCode(code) : undefined}
    />
  );

  return (
    <>
      <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth={true}>
        <DialogTitle className={styles.title}>
          <span>{Messages.codes()}</span>
          <span className={styles.game}>
            {game}
            {gameId && <span className={styles.gameId}>{gameId}</span>}
          </span>
          <span className={styles.titleRight}>
            {locked && (
              <Tooltip title={Messages.locked()}>
                <LockOutlinedIcon className={styles.lock} />
              </Tooltip>
            )}
            <span className={styles.onCount}>{Messages.onCount(codes.on.length)}</span>
          </span>
        </DialogTitle>
        <DialogContent className={styles.content}>
          <div className={styles.toolbar}>
            <TextField
              size="small"
              placeholder={Messages.search()}
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              sx={{ flex: 1 }}
              InputProps={{
                startAdornment: (
                  <InputAdornment position="start">
                    <SearchRoundedIcon fontSize="small" />
                  </InputAdornment>
                ),
              }}
            />
            <ToggleButtonGroup
              size="small"
              exclusive={true}
              value={filter}
              onChange={(_e, v: "all" | "on" | null) => v && setFilter(v)}
            >
              <ToggleButton value="all">{Messages.all()}</ToggleButton>
              <ToggleButton value="on">{Messages.on()}</ToggleButton>
            </ToggleButtonGroup>
            {!locked && (
              <Tooltip title={Messages.paste()}>
                <IconButton
                  className={styles.add}
                  aria-label={Messages.paste()}
                  onClick={() => setEditing({ code: null })}
                >
                  <AddRoundedIcon />
                </IconButton>
              </Tooltip>
            )}
          </div>
          <div className={styles.list}>
            {lists === null && <p className={styles.empty}>{Messages.loading()}</p>}
            {nothing && !q && filter === "all" && <p className={styles.empty}>{Messages.none()}</p>}
            {sections.map((s) => {
              const items = s.list.filter((c) => shown(c, s.source));
              if (items.length === 0) {
                return null;
              }
              const header = (
                <div className={styles.section}>
                  {s.label}
                  <span className={styles.sectionCount}>{items.length}</span>
                </div>
              );
              return (
                <div key={s.source}>
                  {s.hint ? (
                    <Tooltip title={<span style={{ whiteSpace: "pre-line" }}>{s.hint}</span>} placement="left">
                      {header}
                    </Tooltip>
                  ) : (
                    header
                  )}
                  {items.map((c) => row(s.source, c))}
                </div>
              );
            })}
            {lists !== null && (
              <div>
                <Tooltip title={Messages.databaseHint()} placement="left" disableInteractive={true}>
                  <div
                    className={clsx(styles.section, styles.sectionClickable)}
                    role="button"
                    tabIndex={0}
                    onClick={() => {
                      if (!databaseOpen && database.kind === "idle") {
                        onLoadDatabase();
                      }
                      setDatabaseOpen(!databaseOpen);
                    }}
                  >
                    {Messages.database()}
                    {database.kind === "ready" && <span className={styles.sectionCount}>{database.codes.length}</span>}
                    <ExpandMoreRoundedIcon
                      fontSize="small"
                      className={clsx(styles.chevron, databaseOpen && styles.chevronOpen)}
                    />
                  </div>
                </Tooltip>
                {databaseOpen && database.kind === "loading" && (
                  <div className={styles.sectionNote}>{Messages.loading()}</div>
                )}
                {databaseOpen && database.kind === "failed" && (
                  <div className={styles.sectionNote}>{Messages.databaseFailed(database.error)}</div>
                )}
                {databaseOpen && database.kind === "ready" && database.codes.length === 0 && (
                  <div className={styles.sectionNote}>{Messages.databaseNone()}</div>
                )}
                {/* Folded, the ones on still show: they run. */}
                {databaseList
                  .filter((c) => (databaseOpen || isOn(codes, "database", c)) && shown(c, "database"))
                  .map((c) => row("database", c))}
              </div>
            )}
          </div>
        </DialogContent>
        <DialogActions>
          <Button variant="contained" onClick={onClose}>
            {Messages.done()}
          </Button>
        </DialogActions>
      </Dialog>
      <CodeEditDialog
        open={editing !== null}
        code={editing?.code ?? null}
        taken={(name) => codes.yours.some((c) => c.name === name && !(editing?.code && c.name === editing.code.name))}
        onSave={saveCode}
        onClose={() => setEditing(null)}
      />
    </>
  );
};

/** + (or a code of the player's own, opened): a name and the lines; Gecko or Action Replay is
 * read from what's pasted (an encrypted line is Action Replay's) and can be switched. */
export const CodeEditDialog = ({
  open,
  code,
  taken,
  onSave,
  onClose,
}: {
  open: boolean;
  /** Null: a new code. */
  code: ModCode | null;
  /** Another of the player's codes has this name. */
  taken: (name: string) => boolean;
  onSave: (code: ModCode) => void;
  onClose: () => void;
}) => {
  const [name, setName] = useState("");
  const [text, setText] = useState("");
  const [kind, setKind] = useState<ModCode["kind"]>("gecko");
  useEffect(() => {
    if (open) {
      setName(code?.name ?? "");
      setText(code?.lines.join("\n") ?? "");
      setKind(code?.kind ?? "gecko");
    }
  }, [open, code]);
  const parsed = text.trim() ? parsePastedCode(text, kind) : null;
  const trimmed = name.trim();
  const ok = trimmed !== "" && !taken(trimmed) && parsed !== null;
  const save = () => {
    if (ok && parsed) {
      onSave({ name: trimmed, kind: parsed.kind, lines: parsed.lines });
    }
  };
  return (
    <Dialog open={open} onClose={onClose} maxWidth="xs" fullWidth={true}>
      <DialogTitle>{code ? Messages.editCode() : Messages.newCode()}</DialogTitle>
      <DialogContent className={styles.editContent}>
        <TextField
          size="small"
          label={Messages.name()}
          value={name}
          autoFocus={true}
          fullWidth={true}
          sx={{ mt: 1 }}
          inputProps={{ maxLength: 128 }}
          onChange={(e) => setName(e.target.value)}
        />
        <TextField
          multiline={true}
          minRows={6}
          maxRows={14}
          value={text}
          fullWidth={true}
          placeholder="04xxxxxx xxxxxxxx"
          error={text.trim() !== "" && parsed === null}
          helperText={text.trim() !== "" && parsed === null ? Messages.notACode() : undefined}
          InputProps={{ classes: { input: styles.mono } }}
          onChange={(e) => {
            setText(e.target.value);
            if (looksLikeActionReplay(e.target.value)) {
              setKind("ar");
            }
          }}
        />
        <ToggleButtonGroup
          size="small"
          exclusive={true}
          value={parsed?.kind ?? kind}
          onChange={(_e, v: ModCode["kind"] | null) => v && setKind(v)}
        >
          <ToggleButton value="gecko">{Messages.gecko()}</ToggleButton>
          <ToggleButton value="ar">{Messages.actionReplay()}</ToggleButton>
        </ToggleButtonGroup>
      </DialogContent>
      <DialogActions>
        <Button color="secondary" onClick={onClose}>
          {Messages.cancel()}
        </Button>
        <Button variant="contained" disabled={!ok} onClick={save}>
          {code ? Messages.save() : Messages.add()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
