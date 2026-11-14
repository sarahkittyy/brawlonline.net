import RuleOutlinedIcon from "@mui/icons-material/RuleOutlined";
import ButtonBase from "@mui/material/ButtonBase";
import Tooltip from "@mui/material/Tooltip";
import { useNavigate } from "react-router-dom";

import { useRememberedRuleset } from "@/lib/rulesets/use_remembered_ruleset";
import { PublicRoomsMessages } from "@/pages/home/overview/public_rooms/public_rooms.messages";
import { RulesetBoxMessages } from "@/pages/home/overview/public_rooms/ruleset_box.messages";

import styles from "./play_page.module.css";

/**
 * The ruleset new rooms start with, left of Create room (docs/design/launcher-redesign.md §7, §8): its icon and name,
 * no box, so the Public rooms row stays on one line (the name gives way first, then all but the icon). A click opens
 * Rulesets, where Use picks another.
 */
export const RulesetChip = () => {
  const { ruleset } = useRememberedRuleset();
  const navigate = useNavigate();
  const r = ruleset?.ruleset;
  const name = r ? `${r.name} v${r.version}` : RulesetBoxMessages.pplusDefault();
  const tooltip = r ? PublicRoomsMessages.rulesetTooltip(r.name, r.version, r.by ?? "") : name;
  return (
    <Tooltip title={tooltip} placement="top" enterDelay={400} disableInteractive={true}>
      <ButtonBase className={styles.ruleset} onClick={() => navigate("/main/rulesets")} data-testid="room-ruleset">
        <RuleOutlinedIcon className={styles.rulesetIcon} />
        <span className={styles.rulesetName}>{name}</span>
      </ButtonBase>
    </Tooltip>
  );
};
