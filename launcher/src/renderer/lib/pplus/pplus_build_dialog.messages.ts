export const PPlusBuildDialogMessages = {
  title: () => "This is not the correct P+ build",
  explain: (cardRelease: string, release: string) => "You're on version {0}, this game runs on version {1}.",
  kept: (cardRelease: string) => "Your {0} SD card is kept next to the new one.",
  cancel: () => "Cancel",
  download: (release: string) => "Download {0}",
  workingTitle: (release: string) => "Getting Project+ {0}",
  errorTitle: (release: string) => "Couldn't get Project+ {0}",
  error: (message: string) => "Something went wrong: {0}",
  stillWrong: (cardRelease: string) => "The SD card is still on {0}.",
  retry: () => "Try again",
};
