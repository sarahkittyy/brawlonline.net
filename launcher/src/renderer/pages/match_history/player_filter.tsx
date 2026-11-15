import { useQuery } from "@tanstack/react-query";
import { useState } from "react";

import { MainPicture } from "@/components/main_picture/main_picture";
import { PlayerSearch } from "@/components/player_search/player_search";
import { useStockIcon } from "@/components/profile/character_icon";
import { useSearchText } from "@/lib/hooks/use_search_text";
import { SpectateMessages } from "@/pages/home/spectate/spectate.messages";
import { useServices } from "@/services";
import type { MatchOpponent } from "@/services/backend/types";

/** Players filtered by at once (the server's limit). */
const MAX_PLAYERS = 4;

/**
 * Match history's player filter (Sarah's board): typing suggests the players the player has played with (their main,
 * name and code); the history shows the matches every tagged player played in.
 */
export const PlayerFilter = ({
  value,
  onChange,
}: {
  value: MatchOpponent[];
  onChange: (players: MatchOpponent[]) => void;
}) => {
  const { backendService } = useServices();
  const [text, setText] = useState("");
  const search = useSearchText(text);
  const full = value.length >= MAX_PLAYERS;
  const { data, isFetching } = useQuery({
    queryKey: ["matchOpponents", search],
    queryFn: () => backendService.fetchOpponents(search),
    enabled: !full,
    staleTime: 30_000,
  });
  return (
    <PlayerSearch
      value={value}
      onChange={onChange}
      options={data ?? []}
      loading={isFetching}
      text={text}
      onTextChange={setText}
      getKey={(p) => p.uid}
      getCode={(p) => p.connectCode}
      getName={(p) => p.displayName}
      renderPicture={(p) => <Picture main={p.main} />}
      placeholder={SpectateMessages.search()}
      max={MAX_PLAYERS}
    />
  );
};

const Picture = ({ main }: { main?: string | null }) => {
  const url = useStockIcon(main ?? "");
  return <MainPicture url={main ? url ?? null : null} size={22} />;
};
