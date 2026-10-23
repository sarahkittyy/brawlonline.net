import type { Meta, StoryObj } from "@storybook/react-webpack5";
import React from "react";

import { LauncherUpdateButton, PlayButton, UpdatingButton } from "./play_button";

const meta = {
  title: "Components/PlayButton",
  component: PlayButton,
} satisfies Meta<typeof PlayButton>;

export default meta;

type Story = StoryObj<typeof meta>;

export const Primary: Story = {
  args: {},
};

export const Disabled: Story = {
  args: {
    disabled: true,
  },
};

export const Updating = () => {
  const [percent, setPercent] = React.useState(0);
  const timer = React.useRef<number | undefined>();

  const startTimer = React.useCallback(() => {
    const incPercent = () => {
      if (percent < 100) {
        setPercent((p) => p + 1);
      } else {
        clearTimeout(timer.current);
      }
    };
    timer.current = window.setTimeout(incPercent, 20);
  }, [percent]);

  const reset = React.useCallback(() => {
    clearTimeout(timer.current);
    setPercent(0);
    startTimer();
  }, [startTimer, setPercent]);

  React.useEffect(() => {
    startTimer();
    return () => {
      clearTimeout(timer.current);
    };
  });

  return (
    <div>
      <UpdatingButton fillPercent={percent / 100} />
      <div>
        <button onClick={reset}>reset</button>
      </div>
    </div>
  );
};

/** A named set-up step at 61%, on the plain frame (before P+'s files give the theme its colours). */
export const ExtractingProjectPlus = () => <UpdatingButton phase="extractProjectPlus" fillPercent={0.61} />;

/** A newer launcher is out and 42% downloaded (the button fills as it downloads). */
export const LauncherUpdateDownloading = () => (
  <LauncherUpdateButton currentVersion="0.1.98" newVersion="0.1.99" downloadProgress={0.427} />
);

/** The newer launcher is downloaded: the button installs it and restarts the launcher. */
export const LauncherUpdateReady = () => <LauncherUpdateButton currentVersion="0.1.98" newVersion="0.1.99" />;
