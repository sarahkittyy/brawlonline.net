import { countryName, SOCIAL_PLATFORMS } from "@common/profile";
import ContentCopyIcon from "@mui/icons-material/ContentCopy";
import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import OpenInNewIcon from "@mui/icons-material/OpenInNew";
import IconButton from "@mui/material/IconButton";
import Popover from "@mui/material/Popover";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import React, { useEffect, useState } from "react";

import { Flag, NoFlag } from "@/components/flag/flag";
import { RankBadge } from "@/components/rank_badge/rank_badge";
import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { formatPosition, formatRating, HIDDEN_RATING } from "@/lib/ranked_format";
import { validateDisplayName } from "@/lib/validate/validate";
import type { PlayerProfile, ProfileSocial, ProfileUpdate, SocialPlatform } from "@/services/backend/types";

import { CharacterIcon, useHasStockIcons } from "./character_icon";
import { CountryPicker, MainsPicker } from "./pickers";
import { ProfileMessages as Messages } from "./profile.messages";
import styles from "./profile.module.css";
import { SocialIcon } from "./social_icons";

/** Mains shown full size (32 px) up to this many, one row; more are half size in two even rows (18 a row at most). */
export const BIG_MAINS = 9;

/** What edit mode does with a change (the dialog's server calls); each rejects with the error when refused. */
export type ProfileEditing = {
  /** The display name, through the same server path as Edit display name (the slur filter applies). */
  rename: (name: string) => Promise<void>;
  /** Mains, country or social media accounts. */
  save: (update: ProfileUpdate) => Promise<void>;
};

/**
 * A player's profile (docs/design/profiles.md): the badge, name, code and rank; mains and country; the Ranked numbers;
 * social media accounts; then the profile's web page. The website's `/profile/<code>` page
 * (server/crates/accounts/src/profile.rs) shows the same fields in the same order; a field added to one goes in the
 * other. Mains, country and socials show only when set, except in edit mode (`editing`, the player's own profile),
 * where each has its editor.
 *
 * `actions` are the buttons under it (Message, Ignore user), none for the player's own profile.
 */
export const ProfileView = React.memo(function ProfileView({
  profile,
  actions,
  onCopyLink,
  onCopyText,
  editing = null,
}: {
  profile: PlayerProfile;
  actions?: React.ReactNode;
  onCopyLink?: (link: string) => void;
  /** Copies a Discord username (it has no page to open). */
  onCopyText?: (text: string) => void;
  editing?: ProfileEditing | null;
}) {
  const r = profile.ranked;
  const mains = profile.mains ?? [];
  const country = profile.country ?? null;
  const socials = profile.socials ?? [];
  const placing = r.rank == null && r.setsPlayed > 0 && r.setsToPlace > 0;
  const standing = r.rank
    ? RankBadgeMessages.rankName(r.rank)
    : placing
    ? Messages.placing(r.setsToPlace)
    : Messages.notRanked();
  const position = formatPosition(r.position, r.rankedPlayers);
  const showAbout = editing != null || mains.length > 0 || country != null;
  return (
    <div className={clsx(styles.profile, editing && styles.editing)} data-testid="profile-view">
      <div className={styles.who} data-tier={r.rank?.tier ?? "none"}>
        {(r.rank || placing) && (
          <div className={styles.badge}>
            <RankBadge rank={r.rank} setsToPlace={placing ? r.setsToPlace : 0} size="large" />
          </div>
        )}
        <div className={styles.names}>
          {editing ? (
            <NameInput name={profile.displayName} onRename={editing.rename} />
          ) : (
            <div className={styles.name} data-testid="profile-name">
              {profile.displayName}
            </div>
          )}
          {profile.connectCode && <div className={styles.code}>{profile.connectCode}</div>}
          <div className={styles.standing} data-testid="profile-standing">
            {standing}
          </div>
        </div>
      </div>

      {showAbout && (
        <div className={styles.about}>
          {(mains.length > 0 || editing) && <MainsRow mains={mains} editing={editing} />}
          {(country || editing) && <CountryRow country={country} editing={editing} />}
        </div>
      )}

      <div className={styles.section}>
        <div className={styles.sectionTitle}>{Messages.ranked()}</div>
        <dl className={styles.stats}>
          {(r.rating != null || r.setsPlayed > 0) && (
            <Stat label={Messages.rating()} value={r.rating != null ? formatRating(r.rating) : HIDDEN_RATING} />
          )}
          {position && <Stat label={Messages.leaderboard()} value={position} />}
          <Stat label={Messages.setsPlayed()} value={String(r.setsPlayed)} />
          {r.setsPlayed > 0 && <Stat label={Messages.record()} value={Messages.recordValue(r.wins, r.losses)} />}
        </dl>
      </div>

      {editing ? (
        <SocialsEditor socials={socials} save={editing.save} />
      ) : (
        socials.length > 0 && <SocialsRow socials={socials} onCopyText={onCopyText} />
      )}

      {(actions || profile.link) && (
        <div className={styles.footer}>
          <div className={styles.actions}>{actions}</div>
          {profile.link && <WebLink link={profile.link} onCopy={onCopyLink} />}
        </div>
      )}
    </div>
  );
});

const Stat = ({ label, value }: { label: string; value: string }) => (
  <div>
    <dt>{label}</dt>
    <dd>{value}</dd>
  </div>
);

/** The name as a text box (edit mode): Enter or leaving it renames, Escape puts it back. */
const NameInput = ({ name, onRename }: { name: string; onRename: (name: string) => Promise<void> }) => {
  const [value, setValue] = useState(name);
  const [problem, setProblem] = useState<string | null>(null);
  useEffect(() => setValue(name), [name]);
  const commit = () => {
    const next = value.trim();
    if (next === name) {
      setValue(name);
      setProblem(null);
      return;
    }
    const valid = validateDisplayName(next);
    if (valid !== true) {
      setProblem(valid);
      return;
    }
    setProblem(null);
    onRename(next).catch(() => setValue(name));
  };
  return (
    <div>
      <input
        className={clsx(styles.name, styles.nameInput, problem && styles.invalid)}
        value={value}
        maxLength={15}
        aria-label={Messages.displayName()}
        spellCheck={false}
        data-testid="profile-name-input"
        onChange={(e) => setValue(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => {
          if (e.key === "Enter") {
            e.currentTarget.blur();
          } else if (e.key === "Escape") {
            e.stopPropagation();
            setValue(name);
            setProblem(null);
          }
        }}
      />
      {problem && <div className={styles.problem}>{problem}</div>}
    </div>
  );
};

/** A small pencil that opens an editor. */
const EditButton = ({ label, onClick }: { label: string; onClick: (el: HTMLElement) => void }) => (
  <Tooltip title={label}>
    <IconButton size="small" className={styles.pencil} aria-label={label} onClick={(e) => onClick(e.currentTarget)}>
      <EditOutlinedIcon sx={{ fontSize: 16 }} />
    </IconButton>
  </Tooltip>
);

/**
 * "Mains:" and the mains, the primary first: up to BIG_MAINS stock icons full size in a row, more at half size in
 * two rows; name tags (wrapping) without a disc. Edit mode: a pencil before "Mains:" opens the picker.
 */
const MainsRow = ({ mains, editing }: { mains: string[]; editing: ProfileEditing | null }) => {
  const icons = useHasStockIcons();
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const small = mains.length > BIG_MAINS;
  return (
    <div className={styles.row} data-testid="profile-mains">
      {editing && <EditButton label={Messages.editMains()} onClick={setAnchor} />}
      <span className={styles.label}>{Messages.mains()}</span>
      {mains.length === 0 ? (
        <span className={styles.none}>{Messages.none()}</span>
      ) : (
        <div
          className={clsx(icons ? styles.mainIcons : styles.mainTags, small && styles.small)}
          // Half size: two even rows (10 mains are two rows of 5).
          style={icons && small ? { gridTemplateColumns: `repeat(${Math.ceil(mains.length / 2)}, 16px)` } : undefined}
        >
          {mains.map((m, i) => (
            <CharacterIcon key={m} character={m} size={small ? 16 : 32} primary={i === 0} forceTag={!icons} />
          ))}
        </div>
      )}
      {editing && (
        <Popover
          open={anchor != null}
          anchorEl={anchor}
          onClose={() => setAnchor(null)}
          anchorOrigin={{ vertical: "bottom", horizontal: "left" }}
        >
          <MainsPicker
            initial={mains}
            onDone={(next) => {
              setAnchor(null);
              if (next.join() !== mains.join()) {
                void editing.save({ mains: next }).catch(() => undefined);
              }
            }}
          />
        </Popover>
      )}
    </div>
  );
};

/** "Country:", the flag and the name. Edit mode: the flag (dashed without one) and a pencil, one button, open the picker. */
const CountryRow = ({ country, editing }: { country: string | null; editing: ProfileEditing | null }) => {
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const name = country ? countryName(country) ?? country : null;
  const flag = country ? <Flag code={country} height={18} title={name ?? undefined} /> : <NoFlag height={18} />;
  return (
    <div className={styles.row} data-testid="profile-country">
      <span className={styles.label}>{Messages.country()}</span>
      {editing ? (
        <button
          type="button"
          className={styles.countryButton}
          aria-label={Messages.editCountry()}
          title={Messages.editCountry()}
          onClick={(e) => setAnchor(e.currentTarget)}
        >
          {flag}
          {name && <span>{name}</span>}
          <EditOutlinedIcon sx={{ fontSize: 16 }} />
        </button>
      ) : (
        <span className={styles.country}>
          {flag}
          <span>{name}</span>
        </span>
      )}
      {editing && (
        <Popover
          open={anchor != null}
          anchorEl={anchor}
          onClose={() => setAnchor(null)}
          anchorOrigin={{ vertical: "bottom", horizontal: "left" }}
        >
          <CountryPicker
            current={country}
            onPick={(code) => {
              setAnchor(null);
              if (code !== country) {
                void editing.save({ country: code }).catch(() => undefined);
              }
            }}
          />
        </Popover>
      )}
    </div>
  );
};

/** The social media accounts set, one logo each: a page opens in the browser; Discord's username is copied. */
const SocialsRow = ({ socials, onCopyText }: { socials: ProfileSocial[]; onCopyText?: (text: string) => void }) => (
  <div className={styles.socials} data-testid="profile-socials">
    {socials.map((s) => {
      const name = SOCIAL_PLATFORMS.find((p) => p.platform === s.platform)?.name ?? s.platform;
      const label = `${name}: ${s.display}`;
      return (
        <Tooltip key={s.platform} title={s.url ? label : `${label} · ${Messages.clickToCopy()}`}>
          <IconButton
            size="small"
            aria-label={label}
            className={styles.socialButton}
            onClick={() => {
              if (s.url) {
                window.electron.shell.openExternal(s.url).catch(() => undefined);
              } else {
                onCopyText?.(s.handle);
              }
            }}
          >
            <SocialIcon platform={s.platform} size={20} />
          </IconButton>
        </Tooltip>
      );
    })}
  </div>
);

/** Edit mode: each platform's logo and a text box (username, @username or link); leaving it or Enter saves it, blank removes it. */
const SocialsEditor = ({
  socials,
  save,
}: {
  socials: ProfileSocial[];
  save: (update: ProfileUpdate) => Promise<void>;
}) => (
  <div className={styles.socialsEditor} data-testid="profile-socials-editor">
    {SOCIAL_PLATFORMS.map(({ platform, name, placeholder }) => (
      <SocialField
        key={platform}
        platform={platform}
        name={name}
        placeholder={placeholder}
        value={socials.find((s) => s.platform === platform)?.handle ?? ""}
        save={save}
      />
    ))}
  </div>
);

const SocialField = ({
  platform,
  name,
  placeholder,
  value,
  save,
}: {
  platform: SocialPlatform;
  name: string;
  placeholder: string;
  value: string;
  save: (update: ProfileUpdate) => Promise<void>;
}) => {
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  const commit = () => {
    if (text.trim() === value) {
      setText(value);
      return;
    }
    save({ socials: { [platform]: text.trim() || null } }).catch(() => setText(value));
  };
  return (
    <label className={styles.socialField} title={name}>
      <SocialIcon platform={platform} size={18} />
      <input
        className={styles.socialInput}
        value={text}
        placeholder={placeholder}
        aria-label={name}
        spellCheck={false}
        data-testid={`social-input-${platform}`}
        onChange={(e) => setText(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => {
          if (e.key === "Enter") {
            e.currentTarget.blur();
          } else if (e.key === "Escape") {
            e.stopPropagation();
            setText(value);
          }
        }}
      />
    </label>
  );
};

/** The profile's web page: its address (opens in the browser) and a copy button. */
const WebLink = ({ link, onCopy }: { link: string; onCopy?: (link: string) => void }) => {
  const shown = link.replace(/^https?:\/\//, "");
  return (
    <div className={styles.webLink}>
      <Tooltip title={Messages.openInBrowser()}>
        <a
          href={link}
          onClick={(e) => {
            e.preventDefault();
            window.electron.shell.openExternal(link).catch(() => undefined);
          }}
        >
          {shown}
          <OpenInNewIcon sx={{ fontSize: 13, marginLeft: "4px", verticalAlign: "-2px" }} />
        </a>
      </Tooltip>
      {onCopy && (
        <Tooltip title={Messages.copyLink()}>
          <IconButton size="small" aria-label={Messages.copyLink()} onClick={() => onCopy(link)}>
            <ContentCopyIcon sx={{ fontSize: 15 }} />
          </IconButton>
        </Tooltip>
      )}
    </div>
  );
};
