import type { Meta } from "@storybook/react-webpack5";
import React from "react";

import { useDolphinStore } from "@/lib/dolphin/use_dolphin_store";

import { PPlusBuildDialog, usePPlusBuildDialog } from "./pplus_build_dialog";

const meta = {
  title: "Components/PPlusBuildDialog",
  component: PPlusBuildDialog,
} satisfies Meta<typeof PPlusBuildDialog>;

export default meta;

const build = { cardRelease: "v3.1.5", release: "v3.2.1" };

export const Ask = () => {
  React.useEffect(() => usePPlusBuildDialog.setState({ build, view: { kind: "ask" } }), []);
  return <PPlusBuildDialog />;
};

export const Downloading = () => {
  React.useEffect(() => {
    useDolphinStore.setState({
      netplayDownloadProgress: { current: 812_000_000, total: 1_854_400_373, phase: "downloadProjectPlus" },
    });
    usePPlusBuildDialog.setState({ build, view: { kind: "working" } });
  }, []);
  return <PPlusBuildDialog />;
};

export const Failed = () => {
  React.useEffect(
    () =>
      usePPlusBuildDialog.setState({
        build,
        view: { kind: "error", message: "Something went wrong: The Project+ v3.2.1 download is corrupt." },
      }),
    [],
  );
  return <PPlusBuildDialog />;
};
