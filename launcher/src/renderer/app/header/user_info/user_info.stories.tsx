import ButtonBase from "@mui/material/ButtonBase";
import { clsx } from "clsx";

import { MAIN_PICTURE_HOVER } from "@/components/main_picture/main_picture";

import menuStyles from "../user_menu/user_menu.module.css";
import { UserInfo } from "./user_info";

export default {
  title: "containers/Header/UserInfo",
  // More on argTypes: https://storybook.js.org/docs/react/api/argtypes
  argTypes: {},
};

// In the header's button, as user_menu.tsx has it: hovering shows the pencil in the empty picture.
const InButton = ({ glow = false, children }: { glow?: boolean; children: React.ReactNode }) => (
  <ButtonBase className={clsx(menuStyles.userInfoButton, MAIN_PICTURE_HOVER, glow && menuStyles.profileGlow)}>
    {children}
  </ButtonBase>
);

// No main set (or no disc): Fox's dotted outline; hover for the pencil.
export const StandardUserInfo = () => (
  <InButton>
    <UserInfo displayName="Player name" picture={null} connectCode="TEST#000" />
  </InButton>
);

export const LoadingUserInfo = () => {
  return <UserInfo displayName="Player name" picture={undefined} connectCode="TEST#000" loading={true} />;
};

export const ErrorUserInfo = () => (
  <InButton>
    <UserInfo
      displayName="Player name"
      picture={null}
      connectCode="TEST#000"
      errorBorder={true}
      errorMessage="Some error message"
    />
  </InButton>
);

// The header's name until this client's player has clicked their own profile's edit pencil (user_menu.tsx)
export const ProfileGlowUserInfo = () => (
  <InButton glow={true}>
    <UserInfo displayName="Player name" picture={null} connectCode="TEST#000" />
  </InButton>
);
