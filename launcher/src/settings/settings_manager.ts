import { DolphinLaunchType } from "@dolphin/types";
import { Mutex } from "async-mutex";
import electronSettings from "electron-settings";
import { EventEmitter } from "events";
import fs from "fs";
import merge from "lodash/merge";
import set from "lodash/set";

import { defaultAppSettings } from "./default_settings";
import { ipc_settingChangedEvent } from "./ipc";
import type { AppSettings, RootSettingsSchema, SettingKey, SettingsSchema, SettingUpdate } from "./types";

electronSettings.configure({
  fileName: "Settings",
  prettify: true,
});

/**
 * Event emitted when a setting changes
 */
export interface SettingChangeEvent<K extends SettingKey = SettingKey> {
  key: K;
  value: K extends keyof SettingsSchema
    ? SettingsSchema[K]
    : K extends keyof RootSettingsSchema
    ? RootSettingsSchema[K]
    : any;
  previousValue: any;
}

/**
 * Type-safe event map for SettingsManager
 * This allows TypeScript to know what events exist and their payload types
 */
export interface SettingsManagerEvents {
  settingChange: (event: SettingChangeEvent) => void;
}

/**
 * Type-safe SettingsManager that emits properly typed events
 * Uses declaration merging to add type-safe event methods
 */
// eslint-disable-next-line @typescript-eslint/no-unsafe-declaration-merging
export interface SettingsManager {
  on<E extends keyof SettingsManagerEvents>(event: E, listener: SettingsManagerEvents[E]): this;
  off<E extends keyof SettingsManagerEvents>(event: E, listener: SettingsManagerEvents[E]): this;
  emit<E extends keyof SettingsManagerEvents>(event: E, ...args: Parameters<SettingsManagerEvents[E]>): boolean;
}

// eslint-disable-next-line @typescript-eslint/no-unsafe-declaration-merging
export class SettingsManager extends EventEmitter {
  // This only stores the actually modified settings
  private appSettings: Partial<AppSettings>;
  private setMutex: Mutex;

  constructor() {
    super();
    this.setMutex = new Mutex();
    const restoredSettings = electronSettings.getSync() as Partial<AppSettings>;

    // If the ISO file no longer exists, don't restore it
    if (restoredSettings.settings?.isoPath) {
      if (!fs.existsSync(restoredSettings.settings.isoPath)) {
        restoredSettings.settings.isoPath = null;
      }
    }
    this.appSettings = restoredSettings;
  }

  /**
   * Subscribe to changes for a specific setting
   * Returns an unsubscribe function
   *
   * @example
   * const unsubscribe = settingsManager.onSettingChange("isoPath", (event) => {
   *   console.log("ISO path changed from", event.previousValue, "to", event.value);
   *   // event.value is automatically typed as string | null ✓
   * });
   */
  onSettingChange<K extends SettingKey>(
    key: K,
    callback: (
      value: K extends keyof SettingsSchema
        ? SettingsSchema[K]
        : K extends keyof RootSettingsSchema
        ? RootSettingsSchema[K]
        : never,
      previousValue: K extends keyof SettingsSchema
        ? SettingsSchema[K]
        : K extends keyof RootSettingsSchema
        ? RootSettingsSchema[K]
        : never,
    ) => void | Promise<void>,
  ): () => void {
    const listener = (event: SettingChangeEvent) => {
      if (event.key === key) {
        void Promise.resolve(callback(event.value as any, event.previousValue as any));
      }
    };

    this.on("settingChange", listener);

    return () => {
      this.off("settingChange", listener);
    };
  }

  get(): AppSettings {
    // Join the settings with our default settings.
    // This allows us to change the defaults without persisting them
    // into the storage.
    return merge({}, defaultAppSettings, this.appSettings);
  }

  getRootSlpPath(): string {
    return this.get().settings.rootSlpPath;
  }

  getEnableNetplayReplays(): boolean {
    return this.get().settings.enableNetplayReplays;
  }

  getEnableMonthlySubfolders(): boolean {
    return this.get().settings.useMonthlySubfolders;
  }

  getInputDelay(): number {
    return this.get().settings.inputDelay;
  }

  getDolphinPath(type: DolphinLaunchType): string | null {
    const settings = this.get().settings;
    switch (type) {
      case DolphinLaunchType.NETPLAY:
        return settings.netplayDolphinPath;
      case DolphinLaunchType.PLAYBACK:
        return settings.playbackDolphinPath ?? settings.netplayDolphinPath;
    }
  }

  /**
   * NEW: Generic setting update method
   * Updates a single setting and emits change event
   */
  async updateSetting<K extends SettingKey>(
    key: K,
    value: K extends keyof SettingsSchema
      ? SettingsSchema[K]
      : K extends keyof RootSettingsSchema
      ? RootSettingsSchema[K]
      : never,
  ): Promise<void> {
    // Determine the object path (settings are nested, root-level are not)
    const objectPath = this.isNestedSetting(key) ? `settings.${key}` : key;

    // Get previous value for the event
    const currentSettings = this.get();
    const previousValue: any = this.isNestedSetting(key)
      ? currentSettings.settings[key as keyof SettingsSchema]
      : (currentSettings as any)[key];

    if (previousValue === value) {
      // No change, so do nothing
      return;
    }

    // Update the setting value
    await this.setMutex.acquire();
    await electronSettings.set(objectPath, value as any);
    set(this.appSettings, objectPath, value);

    // Emit change event for subscribers (side effects handled by subscribers)
    this.emit("settingChange", { key, value, previousValue });

    // Broadcast incremental change to renderer windows
    await ipc_settingChangedEvent.main!.trigger({ updates: [{ key, value }] });
    this.setMutex.release();
  }

  /**
   * NEW: Batch update multiple settings
   * More efficient than calling updateSetting multiple times
   */
  async updateSettings(updates: SettingUpdate[]): Promise<void> {
    await this.setMutex.acquire();

    // Get current settings for previous values
    const currentSettings = this.get();

    // Update all values
    for (const update of updates) {
      const objectPath = this.isNestedSetting(update.key) ? `settings.${update.key}` : update.key;
      await electronSettings.set(objectPath, update.value as any);
      set(this.appSettings, objectPath, update.value);
    }

    // Emit change events for all updates (side effects handled by subscribers)
    for (const update of updates) {
      const previousValue: any = this.isNestedSetting(update.key)
        ? currentSettings.settings[update.key as keyof SettingsSchema]
        : (currentSettings as any)[update.key];

      this.emit("settingChange", { key: update.key, value: update.value, previousValue });
    }

    // Single incremental broadcast for all changes
    await ipc_settingChangedEvent.main!.trigger({ updates });
    this.setMutex.release();
  }

  /**
   * Check if a setting is nested under "settings" or at root level
   * Automatically stays in sync with SettingsSchema by checking against defaultAppSettings
   */
  private isNestedSetting(key: SettingKey): key is keyof SettingsSchema {
    return key in defaultAppSettings.settings;
  }
}
