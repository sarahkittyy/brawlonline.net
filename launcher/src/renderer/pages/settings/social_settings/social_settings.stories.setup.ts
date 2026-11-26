/**
 * Storybook only: the settings store reads the main process's settings when it loads
 * (use_settings.ts), so the stories' window gets a stand-in first (each story sets its own Quick
 * Chat messages and Disable Chat).
 */
const w = window as any;
w.electron ??= {};
w.electron.settings ??= {
  getAppSettingsSync: () => ({
    previousVersion: null,
    settings: { disableChat: false, quickChat: Array(16).fill("") },
  }),
  updateSettings: async (updates: { key: string; value: unknown }[]) => {
    const { useSettingsStore } = await import("@/lib/hooks/use_settings");
    useSettingsStore.getState().applyUpdates(updates as never);
  },
};

export {};
