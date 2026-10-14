import { css } from "@emotion/react";
import styled from "@emotion/styled";
import React from "react";

import { getColor } from "@/lib/player_colors";
import { getCharacterIcon } from "@/lib/utils";

type CommonPlayerBadgeProps = {
  characterId?: number;
  characterColor?: number;
  port: number;
  teamId?: number;
  isWinner?: boolean;
};

// Pre-styled components for better performance
const BadgeContainer = styled.div<{ color: string; isWinner: boolean }>`
  position: relative;
  display: inline-flex;
  align-items: center;
  margin-left: 12px;
  background-color: ${(p) => p.color};
  border-radius: 100px;
  font-size: 13px;
  font-weight: 500;
  padding: 2px;
  ${(p) => (p.isWinner ? `box-shadow: 0px 0px 10px var(--accent-primary);` : "")}
`;

const CharIcon = styled.img`
  position: absolute;
  left: 0;
  width: 24px;
  margin-left: -12px;
`;

const BadgeContent = styled.div`
  background-color: rgba(0, 0, 0, 0.4);
  padding: 5px 15px;
  border-radius: 100px;
  white-space: nowrap;
`;

// Sits on the badge's top-right edge, where Slippi puts its crown.
const CrownContainer = styled.div`
  position: absolute;
  top: -9px;
  right: -2px;
  line-height: 0;
  pointer-events: none;
`;

// Our own drawing (Slippi's crown image left with their art): a gold crown with a dark
// outline, so it reads on every port color.
const WinnerCrown = () => (
  <svg width={20} height={16} viewBox="0 0 20 16" role="img" aria-label="winner">
    <path
      d="M2.5 12.5 L1.5 4 L6.5 7.5 L10 1.5 L13.5 7.5 L18.5 4 L17.5 12.5 Z M2.5 13.5 H17.5 V15 H2.5 Z"
      fill="#f5c542"
      stroke="#6b4a00"
      strokeWidth={1}
      strokeLinejoin="round"
    />
  </svg>
);

const InternalPlayerBadge = ({
  port,
  characterColor,
  characterId,
  teamId,
  isWinner,
  children,
}: React.PropsWithChildren<CommonPlayerBadgeProps>) => {
  const charIcon = getCharacterIcon(characterId, characterColor);
  const color = getColor(port, teamId);

  return (
    <BadgeContainer color={color} isWinner={!!isWinner}>
      <CharIcon src={charIcon} />
      <BadgeContent>{children}</BadgeContent>
      {isWinner && (
        <CrownContainer>
          <WinnerCrown />
        </CrownContainer>
      )}
    </BadgeContainer>
  );
};

type PlayerBadgeProps = CommonPlayerBadgeProps & {
  variant?: "code" | "tag";
  text: string;
};

export const PlayerBadge = React.memo(function PlayerBadge(props: PlayerBadgeProps) {
  const { variant = "tag", text, ...rest } = props;
  if (variant === "tag") {
    return (
      <InternalPlayerBadge {...rest}>
        <span>{text}</span>
      </InternalPlayerBadge>
    );
  }

  const [prefix, suffix] = text.split("#");
  return (
    <InternalPlayerBadge {...rest}>
      <span>{prefix}</span>
      <span
        css={css`
          color: rgba(255, 255, 255, 0.6);
        `}
      >
        #{suffix}
      </span>
    </InternalPlayerBadge>
  );
});
