import Table from "@mui/material/Table";
import TableBody from "@mui/material/TableBody";
import TableCell from "@mui/material/TableCell";
import TableHead from "@mui/material/TableHead";
import TableRow from "@mui/material/TableRow";
import React from "react";

import type { MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import { MatchRow } from "./profile_page";
import { ProfilePageMessages as Messages } from "./profile_page.messages";

export default {
  title: "pages/Profile/MatchRow",
  argTypes: {},
};

const ME = "me";
const none = { ratingBefore: null, ratingAfter: null, ratingChange: null };
const p = (uid: string, name: string, code: string, extra: Partial<MatchHistoryPlayer> = {}): MatchHistoryPlayer => ({
  uid,
  displayName: name,
  connectCode: code,
  wins: 0,
  ...none,
  ...extra,
});
const base = { createdAt: "2026-10-11T02:00:00Z", status: "ASSIGNED", winner: null, endReason: null };

/** Every kind of row: a ranked set, a Direct match and a room's 1v1, free-for-all and team battle. */
const rows: MatchHistoryItem[] = [
  {
    ...base,
    matchId: "ranked",
    mode: "ranked",
    status: "COMPLETE",
    ranked: true,
    winner: ME,
    players: [
      p(ME, "Sarah", "SARA#123", { wins: 2, ratingBefore: 1512.4, ratingAfter: 1528.1, ratingChange: 15.7 }),
      p("o", "Knuckles", "KNUX#9", { wins: 1, ratingBefore: 1540, ratingAfter: 1524.3, ratingChange: -15.7 }),
    ],
  },
  {
    ...base,
    matchId: "direct",
    mode: "direct",
    ranked: false,
    players: [p(ME, "Sarah", "SARA#123", { wins: 3 }), p("o", "Drsmsh", "DRSM#41", { wins: 2 })],
  },
  {
    ...base,
    matchId: "room-1v1",
    mode: "room",
    ranked: false,
    room: { code: "KFQB", mode: "1v1" },
    players: [
      p(ME, "Sarah", "SARA#123", { wins: 1, port: 1, place: 1 }),
      p("o", "Wario Fan", "WARI#7", { port: 3, place: 2 }),
    ],
  },
  {
    ...base,
    matchId: "room-ffa",
    mode: "room",
    ranked: false,
    room: { code: "TRZM", mode: "ffa" },
    players: [
      p("a", "Wario Fan", "WARI#7", { port: 1, place: 3 }),
      p(ME, "Sarah", "SARA#123", { port: 2, place: 2 }),
      p("b", "Ness", "NESS#12", { wins: 1, port: 3, place: 1 }),
      p("c", "ネス", "NESU#3", { port: 4, place: null }),
    ],
  },
  {
    ...base,
    matchId: "room-teams",
    mode: "room",
    ranked: false,
    room: { code: "PLNX", mode: "teams" },
    players: [
      p("a", "Wario Fan", "WARI#7", { port: 1, team: 0, place: 2 }),
      p(ME, "Sarah", "SARA#123", { wins: 1, port: 2, team: 1, place: 1 }),
      p("b", "Ness", "NESS#12", { port: 3, team: 0, place: 2 }),
      p("c", "Lucas", "LUCA#5", { wins: 1, port: 4, team: 1, place: 1 }),
    ],
  },
];

export const AllModes = () => (
  <Table size="small">
    <TableHead>
      <TableRow>
        <TableCell>{Messages.mode()}</TableCell>
        <TableCell>{Messages.date()}</TableCell>
        <TableCell>{Messages.you()}</TableCell>
        <TableCell align="center">{Messages.score()}</TableCell>
        <TableCell>{Messages.opponent()}</TableCell>
      </TableRow>
    </TableHead>
    <TableBody>
      {rows.map((m) => (
        <MatchRow key={m.matchId} item={m} myUid={ME} />
      ))}
    </TableBody>
  </Table>
);
