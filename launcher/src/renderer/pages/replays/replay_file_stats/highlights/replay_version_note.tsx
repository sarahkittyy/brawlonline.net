// The stats page's line about a replay from another Project+ release (docs/design/pplus-versions.md):
// what it plays on, and getting that release's files now instead of at the first play.
import { pplusReleaseLabel } from "@common/pplus_releases";
import CheckCircleIcon from "@mui/icons-material/CheckCircle";
import HistoryIcon from "@mui/icons-material/History";
import Button from "@mui/material/Button";
import { useState } from "react";

import { formatBytes } from "@/lib/pplus/replay_version_dialog";
import { useOlderRelease, usePPlusVersions } from "@/lib/pplus/use_pplus_versions";
import { ensureReplayRelease } from "@/lib/pplus/with_replay_version";

import { HighlightListMessages as Messages } from "./highlight_list.messages";
import styles from "./highlight_list.module.css";

export const ReplayVersionNote = ({ release }: { release: string | undefined }) => {
  const state = useOlderRelease(release);
  const current = usePPlusVersions((s) => s.current);
  const [busy, setBusy] = useState(false);
  if (!state || !release || !current) {
    return null;
  }
  const version = pplusReleaseLabel(release);
  const mine = pplusReleaseLabel(current);
  if (state === "unknown" || !state.playable) {
    return (
      <div className={styles.versionNote}>
        <HistoryIcon fontSize="small" />
        <span>{Messages.versionUnplayable(version)}</span>
      </div>
    );
  }
  if (state.filesReady) {
    return (
      <div className={styles.versionNote}>
        <CheckCircleIcon fontSize="small" color="primary" />
        <span>{Messages.versionReady(version)}</span>
      </div>
    );
  }
  const getNow = async () => {
    setBusy(true);
    try {
      await ensureReplayRelease({ release, current, downloadBytes: state.downloadBytes });
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className={styles.versionNote}>
      <HistoryIcon fontSize="small" />
      <span>{Messages.versionNeeded(version, mine, formatBytes(state.downloadBytes || 51_400_000))}</span>
      <Button
        size="small"
        variant="outlined"
        disabled={busy}
        onClick={() => void getNow()}
        style={{ marginLeft: "auto", whiteSpace: "nowrap", flexShrink: 0 }}
      >
        {Messages.versionGetNow()}
      </Button>
    </div>
  );
};
