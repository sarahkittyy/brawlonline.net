import ButtonBase from "@mui/material/ButtonBase";
import { clsx } from "clsx";

import { generateDisplayPicture } from "@/lib/display_picture";

import menuStyles from "../user_menu/user_menu.module.css";
import { UserInfo } from "./user_info";

// type UserInfoProps = React.ComponentProps<typeof UserInfo>;

export default {
  title: "containers/Header/UserInfo",
  // More on argTypes: https://storybook.js.org/docs/react/api/argtypes
  argTypes: {},
};

const displayPicture = generateDisplayPicture("");

export const StandardUserInfo = () => {
  return <UserInfo displayName="Player name" displayPicture={displayPicture} connectCode="TEST#000" />;
};

export const LoadingUserInfo = () => {
  return <UserInfo displayName="Player name" displayPicture={displayPicture} connectCode="TEST#000" loading={true} />;
};

export const ErrorUserInfo = () => {
  return (
    <UserInfo
      displayName="Player name"
      displayPicture={displayPicture}
      connectCode="TEST#000"
      errorMessage="Some error message"
    />
  );
};

// The header's name until this client's player has clicked their own profile's edit pencil (user_menu.tsx)
export const ProfileGlowUserInfo = () => {
  return (
    <ButtonBase className={clsx(menuStyles.userInfoButton, menuStyles.profileGlow)}>
      <UserInfo displayName="Player name" displayPicture={displayPicture} connectCode="TEST#000" />
    </ButtonBase>
  );
};
