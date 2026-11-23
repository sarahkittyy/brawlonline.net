import { useQuery, useQueryClient } from "@tanstack/react-query";

import { UserName } from "@/components/user_name/user_name";
import { getColor } from "@/lib/player_colors";
import { getCharacterIcon } from "@/lib/utils";
import { useServices } from "@/services";

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

/**
 * The account a replay's connect code is now, for its name's profile link, hover card and right-click menu (Sarah's
 * board, 2026-11-23). A replay keeps only the code: the profile by code gives the uid, and seeds the uid's own
 * profile query, so the hover card and the dialog have it. Undefined while it loads and for a code no account has
 * now (a code that changed hands, a banned player): the name stays plain text.
 */
const useUidForCode = (connectCode: string | undefined): string | undefined => {
  const { backendService } = useServices();
  const queryClient = useQueryClient();
  const { data } = useQuery({
    queryKey: ["profileByCode", connectCode],
    queryFn: async () => {
      const profile = await backendService.fetchProfile(connectCode!);
      queryClient.setQueryData(["profile", profile.uid], profile);
      return profile;
    },
    enabled: Boolean(connectCode),
    staleTime: 5 * 60_000,
    retry: false,
  });
  // (Only while the code is still that account's: the profile by code is the account's current code.)
  return data && data.connectCode.toUpperCase() === connectCode?.toUpperCase() ? data.uid : undefined;
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
  const uid = useUidForCode(connectCode);
  const name = displayName || tag || backupName;
  return (
    <div className={styles.outer}>
      <div className={styles.iconContainer}>
        <img src={charIcon} />
      </div>
      <div className={styles.infoContainer}>
        <div className={styles.nameRow}>
          {uid ? (
            <UserName user={{ uid, displayName: name, connectCode }} showCode={false} profileLink={true} />
          ) : (
            <span>{name}</span>
          )}
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
