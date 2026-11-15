import styled from "@emotion/styled";
import React from "react";

import { PlayerBadge } from "./player_badge/player_badge";

// Who played, a team at a time (Sarah's board): all on one line when they fit; otherwise a team per line, "vs" leading
// every team after the first, as Highlights and Spectate do. A team only wraps inside itself when it alone is wider
// than the row.
const Outer = styled.div`
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  align-content: center;
  row-gap: 10px;
  min-width: 0;
  min-height: 34px;
`;

const Team = styled.div`
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  row-gap: 10px;
  max-width: 100%;
  & > * {
    margin-right: 8px;
  }
  /* (Nothing after the last player: a 1v1 keeps its line beside the buttons at the launcher's smallest.) */
  &:last-child > :last-child {
    margin-right: 0;
  }
`;

const Vs = styled.div`
  color: rgba(255, 255, 255, 0.5);
`;

export type PlayerInfo = React.ComponentProps<typeof PlayerBadge>;

type TeamElementProps = {
  teams: PlayerInfo[][];
};

export const TeamElements = ({ teams }: TeamElementProps) => {
  return (
    <Outer>
      {teams.map((team, i) => (
        <Team key={`team-${i}`}>
          {i > 0 && <Vs>vs</Vs>}
          {team.map((player) => (
            <PlayerBadge key={`player-${player.port}`} {...player} />
          ))}
        </Team>
      ))}
    </Outer>
  );
};
