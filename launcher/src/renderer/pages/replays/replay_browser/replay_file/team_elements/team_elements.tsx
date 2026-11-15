import styled from "@emotion/styled";
import React from "react";

import { PlayerBadge } from "./player_badge/player_badge";

// Who played (Sarah's board): all on one line when they fit; otherwise, when a team has two players or more, a team per
// line with "vs" leading every team after the first, as Highlights and Spectate do. Never more: a 1v1 (or any game of
// one-player teams) stays on its line and a team never splits; too wide, they run a little under the buttons.
const Outer = styled.div<{ teamPerLine: boolean }>`
  display: flex;
  flex-wrap: ${(p) => (p.teamPerLine ? "wrap" : "nowrap")};
  align-items: center;
  align-content: center;
  row-gap: 10px;
  min-width: 0;
  min-height: 34px;
`;

const Team = styled.div`
  display: flex;
  flex: none;
  align-items: center;
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
    <Outer teamPerLine={teams.some((team) => team.length > 1)}>
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
