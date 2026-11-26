export type SettingItem = {
  name: () => string;
  path: string;
  component: JSX.Element;
  icon?: JSX.Element;
  hidden?: () => boolean;
};

/** An entry that does something when clicked instead of opening a page (Changelog opens its popup). */
export type SettingAction = {
  name: () => string;
  onClick: () => void;
  icon?: JSX.Element;
  hidden?: () => boolean;
};

export type SettingSection = {
  title?: () => string;
  items: (SettingItem | SettingAction)[];
};

export const isSettingAction = (item: SettingItem | SettingAction): item is SettingAction => "onClick" in item;
