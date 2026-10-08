import { PRODUCT_NAME } from "@common/product";
import Button from "@mui/material/Button";
import React from "react";

import { Toggle } from "@/components/form/toggle";
import { useAutoUpdateLauncher } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";

import { SettingItem } from "../setting_item_section";
import { AdvancedAppSettingsMessages as Messages } from "./advanced_app_settings.messages";

export const AdvancedAppSettings = React.memo(() => {
  const [autoUpdateLauncher, setAutoUpdateLauncher] = useAutoUpdateLauncher();

  return (
    <div>
      <SettingItem>
        <Toggle
          value={autoUpdateLauncher}
          onChange={(checked) => setAutoUpdateLauncher(checked)}
          label={Messages.enableAutoUpdates()}
          description={Messages.enableAutoUpdatesDescription(PRODUCT_NAME)}
        />
      </SettingItem>
      <ClearTempFilesForm />
    </div>
  );
});

const ClearTempFilesForm = React.memo(() => {
  const { showSuccess, showError } = useToasts();
  const onClear = React.useCallback(() => {
    window.electron.common
      .clearTempFolder()
      .then(() => {
        showSuccess(Messages.clearFilesSuccess());
      })
      .catch(showError);
  }, [showSuccess, showError]);
  return (
    <SettingItem name={Messages.clearTempFiles()} description={Messages.clearTempFilesDescription()}>
      <Button variant="contained" color="secondary" onClick={onClear}>
        {Messages.clearFiles()}
      </Button>
    </SettingItem>
  );
});
