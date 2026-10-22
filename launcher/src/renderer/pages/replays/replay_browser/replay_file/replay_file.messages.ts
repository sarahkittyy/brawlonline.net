export const ReplayFileMessages = {
  unknownStage: () => "Unknown stage",
  moreOptions: () => "More options",
  showHighlights: () => "Show highlights",
  renderToVideo: () => "Render to video",
  renderWaiting: () => "Waiting to render (click to cancel)",
  renderingToVideo: (percent: number) => "Rendering to video... {0}% (click to cancel)",
  matchRendered: (name: string) => "Rendered {0}",
  matchRenderFailed: (error: string) => "Could not render the match to video: {0}",
  launchReplay: () => "Launch replay",
  dolphinIsUpdating: () => "Dolphin is updating",
  pplusRelease: (version: string) => "Project+ {0}",
};
