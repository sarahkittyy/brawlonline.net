// Settings > Replays > Older Project+ versions (docs/design/pplus-versions.md): the releases kept
// for replays, what they take on disk, download / remove, and "Don't ask again".
import { pplusReleaseLabel } from "@common/pplus_releases";
import type { PPlusVersionInfo } from "@dolphin/types";
import Button from "@mui/material/Button";
import { useEffect, useState } from "react";

import { Toggle } from "@/components/form/toggle";
import { useSetting } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { formatBytes } from "@/lib/pplus/replay_version_dialog";
import { refreshPPlusVersions, usePPlusVersions } from "@/lib/pplus/use_pplus_versions";
import { ensureReplayRelease } from "@/lib/pplus/with_replay_version";

import { SettingItem } from "../setting_item_section";
import { ReplaySettingsMessages as Messages } from "./replay_settings.messages";

export const PPlusVersionsSetting = () => {
  const current = usePPlusVersions((s) => s.current);
  const states = usePPlusVersions((s) => s.states);
  const [autoDownload, setAutoDownload] = useSetting("pplusAutoDownload");
  const [busy, setBusy] = useState<string | null>(null);
  const { showError } = useToasts();

  useEffect(() => {
    void refreshPPlusVersions();
  }, []);

  const older = states.filter((s) => !s.current);
  const get = async (s: PPlusVersionInfo) => {
    if (!current) {
      return;
    }
    setBusy(s.release);
    try {
      await ensureReplayRelease({ release: s.release, current, downloadBytes: s.downloadBytes });
    } finally {
      setBusy(null);
      void refreshPPlusVersions();
    }
  };
  const remove = async (release: string) => {
    setBusy(release);
    try {
      await window.electron.dolphin.removePPlusRelease(release);
    } catch (err) {
      showError(err);
    } finally {
      setBusy(null);
      void refreshPPlusVersions();
    }
  };

  return (
    <SettingItem
      name={Messages.olderVersions()}
      description={Messages.olderVersionsDescription(pplusReleaseLabel(current ?? ""))}
    >
      {older.length === 0 && <div style={{ opacity: 0.7 }}>{Messages.noOlderVersions()}</div>}
      {older.map((s) => {
        const version = pplusReleaseLabel(s.release);
        const status = !s.playable
          ? Messages.versionUnplayable()
          : s.filesReady
          ? Messages.versionReady(formatBytes(s.filesBytes + s.cardBytes))
          : Messages.versionNotHere(formatBytes(s.downloadBytes));
        return (
          <div
            key={s.release}
            style={{
              display: "flex",
              alignItems: "center",
              gap: 12,
              padding: "10px 14px",
              marginBottom: 8,
              borderRadius: 8,
              background: "rgba(0, 0, 0, 0.25)",
            }}
          >
            <div style={{ flex: 1 }}>
              <div style={{ fontWeight: 600 }}>{Messages.versionName(version)}</div>
              <div style={{ fontSize: 13, opacity: 0.8 }}>{status}</div>
            </div>
            {s.playable && !s.filesReady && (
              <Button variant="contained" size="small" disabled={busy != null} onClick={() => void get(s)}>
                {Messages.getVersion()}
              </Button>
            )}
            {(s.filesReady || s.cardReady) && (
              <Button variant="outlined" size="small" disabled={busy != null} onClick={() => void remove(s.release)}>
                {Messages.removeVersion()}
              </Button>
            )}
          </div>
        );
      })}
      <div style={{ marginTop: 8 }}>
        <Toggle
          value={autoDownload}
          onChange={(checked) => void setAutoDownload(checked)}
          label={Messages.autoDownload()}
          description={Messages.autoDownloadDescription()}
        />
      </div>
    </SettingItem>
  );
};
