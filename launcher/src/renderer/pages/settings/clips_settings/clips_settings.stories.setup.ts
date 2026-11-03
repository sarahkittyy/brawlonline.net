/**
 * Storybook only: the settings store reads the main process's settings when it loads
 * (use_settings.ts), so the stories' window gets a stand-in first, with Settings > Clips' settings
 * and an L+R+A hotkey set.
 */
const w = window as any;
w.electron ??= {};
w.electron.settings ??= {
  getAppSettingsSync: () => ({
    previousVersion: null,
    settings: {
      clipOnLrz: true,
      clipHotkeys: [{ buttons: 0x0160, since: 0 }],
      disableHighlightDetection: false,
      clipsFolder: null,
      rootSlpPath: "C:\\Users\\Player\\Documents\\Brawl Online",
      clipResolution: 720,
    },
  }),
  updateSettings: async (updates: { key: string; value: unknown }[]) => {
    const { useSettingsStore } = await import("@/lib/hooks/use_settings");
    useSettingsStore.getState().applyUpdates(updates as never);
  },
};

export {};
