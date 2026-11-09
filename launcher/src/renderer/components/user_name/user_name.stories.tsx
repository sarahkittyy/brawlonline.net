import type { UserNameData } from "./user_name";
import { UserName } from "./user_name";

export default {
  title: "components/UserName",
  argTypes: {},
};

const players: UserNameData[] = [
  { uid: "a", displayName: "Sarah", connectCode: "SARA#123", rank: { tier: "gold", division: 2 } },
  { uid: "b", displayName: "Knuckles", connectCode: "KNUX#9", rank: { tier: "emerald", division: 3 } },
  { uid: "c", displayName: "Drsmsh", connectCode: "DRSM#41", rank: null },
  { uid: "d", displayName: "Lucas", connectCode: "LUCA#5", rank: { tier: "legendary", division: null } },
  { uid: "e", displayName: "Wario Fan", connectCode: "WARI#7", rank: { tier: "silver", division: 1 } },
];

/** As the match history shows names: the rank then, the code, the name opening the profile. */
export const WithBadges = () => (
  <div style={{ display: "flex", flexDirection: "column", gap: 8, padding: 16, fontSize: 14 }}>
    {players.map((p) => (
      <UserName key={p.uid} user={p} showBadge={true} profileLink={true} />
    ))}
  </div>
);

/** Name and code only (the online list, chat). */
export const Plain = () => (
  <div style={{ display: "flex", flexDirection: "column", gap: 8, padding: 16, fontSize: 13 }}>
    {players.map((p) => (
      <UserName key={p.uid} user={p} />
    ))}
  </div>
);
