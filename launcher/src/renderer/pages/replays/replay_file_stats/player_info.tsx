import { getColor } from "@/lib/player_colors";
import { getCharacterIcon } from "@/lib/utils";

import styles from "./player_info.module.css";

type PlayerInfoProps = {
  isTeams: boolean;
  playerIndex: number;
  type?: number;
  teamId?: number;
  characterId?: number;
  characterColor?: number;
  connectCode?: string;
  displayName?: string;
  tag?: string;
};

export const PlayerInfo = ({
  isTeams,
  playerIndex,
  type,
  teamId,
  characterId,
  characterColor,
  connectCode,
  displayName,
  tag,
}: PlayerInfoProps) => {
  const port = playerIndex + 1;
  const backupName = type === 1 ? "CPU" : `Player ${port}`;
  const charIcon = getCharacterIcon(characterId, characterColor);
  return (
    <div className={styles.outer}>
      <div className={styles.iconContainer}>
        <img src={charIcon} />
      </div>
      <div className={styles.infoContainer}>
        <div className={styles.nameRow}>
          <span>{displayName || tag || backupName}</span>
          <span
            className={styles.portBadge}
            style={{
              backgroundColor: getColor(port, isTeams ? teamId : undefined),
            }}
          >
            P{port}
          </span>
        </div>
        {connectCode && (
          <div className={styles.connectCodeRow}>
            {/* Slippi links the code to its slippi.gg profile page; we have no profile pages yet. */}
            <span className={styles.codeLink}>{connectCode}</span>
          </div>
        )}
      </div>
    </div>
  );
};
