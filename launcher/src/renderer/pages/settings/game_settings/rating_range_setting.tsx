import { normalizeRatingRange, RATING_RANGE_MAX, RATING_RANGE_OFF, RATING_RANGE_PRESETS } from "@common/rating_range";
import { css } from "@emotion/react";
import FormControlLabel from "@mui/material/FormControlLabel";
import InputAdornment from "@mui/material/InputAdornment";
import Radio from "@mui/material/Radio";
import RadioGroup from "@mui/material/RadioGroup";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import Typography from "@mui/material/Typography";
import React from "react";

import { RankBadgeMessages } from "@/components/rank_badge/rank_badge.messages";
import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";
import { useAccount } from "@/lib/hooks/use_account";
import { useUnrankedRatingRange } from "@/lib/hooks/use_settings";
import { formatRating, RANK_FLOORS, RANK_TIERS, rankForRating, TIER_COLORS } from "@/lib/ranked_format";
import type { RankedProfile } from "@/services/backend/types";

import { SettingItem } from "../setting_item_section";
import { GameSettingsMessages as Messages } from "./game_settings.messages";

// The range Custom starts with, before the player typed one.
const DEFAULT_CUSTOM_RANGE = 150;
const CUSTOM = "custom";

// The scale the preview draws: Bronze 2's floor to past Legendary's, widened to keep the player's own rating on it.
const SCALE_MIN = 900;
const SCALE_MAX = 2400;

/**
 * "Unranked Rating Range": Off, ±100, ±200, ±300 or a custom number of points, and a preview of
 * the ratings the player would be matched with. Dolphin reads it at start, so it is greyed out while
 * Dolphin is open.
 */
export const RatingRangeSetting = () => {
  const [stored, setStored] = useUnrankedRatingRange();
  const value = normalizeRatingRange(stored);
  const isCustom = value !== RATING_RANGE_OFF && !RATING_RANGE_PRESETS.includes(value);
  const disabled = useDolphinStore((store) => store.netplayOpened);
  const rankedProfile = useAccount((store) => store.userData?.rankedNetplayProfile);

  // Custom's last number, so picking a preset and back comes back to it; and the field's text while typing.
  const lastCustom = React.useRef(isCustom ? value : DEFAULT_CUSTOM_RANGE);
  const [customText, setCustomText] = React.useState(String(lastCustom.current));
  // Custom stays picked while typing a preset's number.
  const [customPicked, setCustomPicked] = React.useState(isCustom);
  const showCustom = isCustom || customPicked;
  React.useEffect(() => {
    if (isCustom) {
      lastCustom.current = value;
      setCustomText(String(value));
    }
  }, [isCustom, value]);

  const onPick = (picked: string) => {
    setCustomPicked(picked === CUSTOM);
    if (picked === CUSTOM) {
      setCustomText(String(lastCustom.current));
    }
    void setStored(picked === CUSTOM ? lastCustom.current : Number(picked));
  };

  const onType = (text: string) => {
    setCustomText(text);
    const n = Number(text);
    if (text.trim() !== "" && Number.isInteger(n) && n > 0) {
      lastCustom.current = normalizeRatingRange(n);
      void setStored(lastCustom.current);
    }
  };

  return (
    <SettingItem name={Messages.unrankedRatingRange()} description={Messages.unrankedRatingRangeDescription()}>
      <Tooltip title={disabled ? Messages.closeDolphinToChange() : ""} placement="bottom-start">
        <div
          css={css`
            display: flex;
            flex-wrap: wrap;
            align-items: center;
            gap: 8px;
          `}
        >
          <RadioGroup row={true} value={showCustom ? CUSTOM : String(value)} onChange={(_event, v) => onPick(v)}>
            {[RATING_RANGE_OFF, ...RATING_RANGE_PRESETS].map((points) => (
              <FormControlLabel
                key={points}
                value={String(points)}
                label={
                  points === RATING_RANGE_OFF
                    ? Messages.unrankedRatingRangeOff()
                    : Messages.unrankedRatingRangePoints(points)
                }
                control={<Radio />}
                disabled={disabled}
              />
            ))}
            <FormControlLabel
              value={CUSTOM}
              label={Messages.unrankedRatingRangeCustom()}
              control={<Radio />}
              disabled={disabled}
            />
          </RadioGroup>
          {showCustom && (
            <TextField
              size="small"
              type="number"
              value={customText}
              disabled={disabled}
              onChange={(e) => onType(e.target.value)}
              onBlur={() => setCustomText(String(value))}
              inputProps={{ min: 1, max: RATING_RANGE_MAX, step: 25 }}
              InputProps={{ startAdornment: <InputAdornment position="start">±</InputAdornment> }}
              css={css`
                width: 120px;
              `}
            />
          )}
        </div>
      </Tooltip>
      {value !== RATING_RANGE_OFF && <RatingRangePreview range={value} profile={rankedProfile} />}
    </SettingItem>
  );
};

/**
 * The ratings a range reaches, on a strip of the rank tiers: the player's rating, and the window
 * mm pairs them within. Only placed opponents in that window; until the player places, nothing.
 */
const RatingRangePreview = ({ range, profile }: { range: number; profile: RankedProfile | undefined }) => {
  const caption = css`
    margin-top: 8px;
    opacity: 0.8;
  `;
  if (!profile) {
    return (
      <Typography variant="body2" css={caption}>
        {Messages.unrankedRatingRangeNoProfile()}
      </Typography>
    );
  }
  const setsToPlace = profile.setsToPlace ?? 0;
  if (setsToPlace > 0) {
    return (
      <Typography variant="body2" css={caption}>
        {Messages.unrankedRatingRangeNotPlaced(setsToPlace)}
      </Typography>
    );
  }

  const { rating } = profile;
  const low = Math.max(0, rating - range);
  const high = rating + range;
  const scaleMin = Math.min(SCALE_MIN, rating - 100);
  const scaleMax = Math.max(SCALE_MAX, rating + 100);
  const at = (r: number) =>
    `${((Math.min(Math.max(r, scaleMin), scaleMax) - scaleMin) / (scaleMax - scaleMin)) * 100}%`;

  // Each tier's span on the scale: from its lowest division's floor to the next tier's.
  const tiers = RANK_TIERS.map((tier, i) => ({
    tier,
    from: i === 0 ? scaleMin : RANK_FLOORS[i * 3],
    to: i + 1 < RANK_TIERS.length ? RANK_FLOORS[(i + 1) * 3] : scaleMax,
  }));

  return (
    <div
      css={css`
        margin-top: 12px;
        max-width: 520px;
      `}
    >
      <div
        css={css`
          position: relative;
          height: 14px;
          border-radius: 4px;
          overflow: hidden;
        `}
      >
        {tiers.map(({ tier, from, to }) => (
          <div
            key={tier}
            title={RankBadgeMessages.rankName({ tier, division: null })}
            css={css`
              position: absolute;
              top: 0;
              bottom: 0;
              left: ${at(from)};
              right: calc(100% - ${at(to)});
              background: ${TIER_COLORS[tier]};
              opacity: 0.3;
            `}
          />
        ))}
        {tiers.map(({ tier, from, to }) => (
          <div
            key={tier}
            css={css`
              position: absolute;
              top: 0;
              bottom: 0;
              left: ${at(Math.max(from, low))};
              right: calc(100% - ${at(Math.min(to, high))});
              background: ${TIER_COLORS[tier]};
              display: ${from < high && to > low ? "block" : "none"};
            `}
          />
        ))}
      </div>
      <div
        css={css`
          position: relative;
          height: 0;
        `}
      >
        <div
          title={formatRating(rating)}
          css={css`
            position: absolute;
            top: -18px;
            left: ${at(rating)};
            width: 2px;
            height: 22px;
            margin-left: -1px;
            background: white;
            box-shadow: 0 0 2px black;
          `}
        />
      </div>
      <Typography variant="body2" css={caption}>
        {Messages.unrankedRatingRangePreview(
          formatRating(rating),
          Math.ceil(low).toString(),
          Math.floor(high).toString(),
          RankBadgeMessages.rankName(rankForRating(low)),
          RankBadgeMessages.rankName(rankForRating(high)),
        )}
      </Typography>
    </div>
  );
};
