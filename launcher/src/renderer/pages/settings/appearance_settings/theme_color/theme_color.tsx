import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import Button from "@mui/material/Button";
import log from "electron-log";
import React from "react";

import { saveThemeColor, useOpenThemeColorPicker } from "@/components/theme_color_picker/use_theme_color_picker";
import { useThemeColor } from "@/lib/hooks/use_settings";
import { useDefaultThemeColor } from "@/styles/game_theme_provider";
import { normalizeThemeColor, PLAIN_THEME_SEED, relativeLuminance } from "@/styles/theme_color";

import { SettingItem } from "../../setting_item_section";
import { ThemeColorMessages as Messages } from "./theme_color.messages";
import styles from "./theme_color.module.css";

/**
 * The theme colour: a swatch of the current colour (the default look's own when none is chosen) with a
 * pencil, which opens the colour wheel over the main page; "Reset to default" beside it once one is chosen.
 */
export const ThemeColorSetting = React.memo(() => {
  const [setting] = useThemeColor();
  const chosen = normalizeThemeColor(setting);
  const defaultColor = useDefaultThemeColor((s) => s.color);
  const openPicker = useOpenThemeColorPicker();
  const shown = chosen ?? defaultColor ?? PLAIN_THEME_SEED;

  return (
    <SettingItem name={Messages.themeColor()} description={Messages.themeColorDescription()} inline={true}>
      {chosen && (
        <Button
          variant="text"
          color="inherit"
          size="small"
          className={styles.reset}
          onClick={() => void saveThemeColor(null).catch(log.error)}
        >
          {Messages.resetToDefault()}
        </Button>
      )}
      <button
        type="button"
        className={styles.swatch}
        style={{ background: shown }}
        aria-label={Messages.changeThemeColor()}
        title={Messages.changeThemeColor()}
        onClick={openPicker}
      >
        <EditOutlinedIcon className={relativeLuminance(shown) > 0.4 ? styles.darkPencil : styles.lightPencil} />
      </button>
    </SettingItem>
  );
});
