import PeopleAltRoundedIcon from "@mui/icons-material/PeopleAltRounded";
import PersonAddAlt1RoundedIcon from "@mui/icons-material/PersonAddAlt1Rounded";
import PersonRemoveAlt1RoundedIcon from "@mui/icons-material/PersonRemoveAlt1Rounded";
import { createSvgIcon } from "@mui/material/utils";

/**
 * The friends icons (docs/design/friends.md): two people mean "friends" everywhere (a friend in the
 * lists, a friends-only room, "you're friends" in a chat window); a person with + adds, with -
 * removes, with a clock is a request waiting for an answer.
 */
export const FriendsIcon = PeopleAltRoundedIcon;
export const AddFriendIcon = PersonAddAlt1RoundedIcon;
export const RemoveFriendIcon = PersonRemoveAlt1RoundedIcon;

/** PersonAddAlt1's person with a small clock where its + is. */
export const PendingFriendIcon = createSvgIcon(
  [
    <circle key="head" cx="9" cy="8" r="4" />,
    <path key="body" d="M9 14c-2.67 0-8 1.34-8 4v1c0 .55.45 1 1 1h14c.55 0 1-.45 1-1v-1c0-2.66-5.33-4-8-4" />,
    <path key="ring" d="M19 5.5a4.5 4.5 0 1 0 0 9a4.5 4.5 0 1 0 0-9zm0 1.4a3.1 3.1 0 1 1 0 6.2a3.1 3.1 0 1 1 0-6.2z" />,
    <path key="hands" d="M18.4 7.7h1.2v2.05l1.4 1.4-.85.85-1.75-1.75z" />,
  ],
  "PendingFriend",
);
