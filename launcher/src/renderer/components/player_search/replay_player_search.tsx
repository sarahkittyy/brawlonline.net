import type { ReplayPlayer } from "@replays/types";
import { useQuery } from "@tanstack/react-query";

import { MainPicture } from "@/components/main_picture/main_picture";
import { useSearchText } from "@/lib/hooks/use_search_text";
import { getCharacterIcon } from "@/lib/utils";
import { useServices } from "@/services";

import { PlayerSearch } from "./player_search";

/** Tags at most. */
const MAX_PLAYERS = 4;

/**
 * Replays' and Highlights' player search: Match history's (player_search.tsx), its suggestions the players of the
 * replays on this computer (the most games first), and what is typed filtering the list too.
 */
export const ReplayPlayerSearch = ({
  value,
  onChange,
  text,
  onTextChange,
  placeholder,
  className,
}: {
  value: ReplayPlayer[];
  onChange: (players: ReplayPlayer[]) => void;
  text: string;
  onTextChange: (text: string) => void;
  placeholder: string;
  className?: string;
}) => {
  const { replayService } = useServices();
  // (This computer's database: no need to wait a second between searches.)
  const search = useSearchText(text, { debounceMs: 120, minIntervalMs: 0 });
  const { data, isFetching } = useQuery({
    queryKey: ["replayPlayers", search],
    queryFn: () => replayService.searchPlayers(search, 8),
    enabled: value.length < MAX_PLAYERS,
    staleTime: 30_000,
  });
  return (
    <PlayerSearch
      value={value}
      onChange={onChange}
      options={data ?? []}
      loading={isFetching}
      text={text}
      onTextChange={onTextChange}
      getKey={(p) => p.connectCode}
      getCode={(p) => p.connectCode}
      getName={(p) => p.displayName}
      renderPicture={(p) => (
        <MainPicture url={p.characterId != null ? getCharacterIcon(p.characterId, p.costume ?? 0) : null} size={22} />
      )}
      placeholder={placeholder}
      max={MAX_PLAYERS}
      testId="replay-player-search"
      className={className}
    />
  );
};
