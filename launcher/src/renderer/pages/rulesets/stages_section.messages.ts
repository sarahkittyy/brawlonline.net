export const StagesMessages = {
  starter: () => "Starter",
  counterpick: () => "Counterpick",
  off: () => "Off",
  otherStages: () => "Other stages",
  otherOn: (n: number) => (n === 0 ? "All off" : `${n} on`),
  hazardsOwnTitle: (on: boolean) =>
    on
      ? "Hazards on for this stage. Click: back to the default."
      : "Hazards off for this stage. Click: back to the default.",
  hazardsDefaultTitle: (on: boolean) =>
    on
      ? "Hazards on (the default). Click: off for this stage."
      : "Hazards off (the default). Click: on for this stage.",
  hazardsFull: (n: number) => `At most ${n} stages can have their own hazards.`,
};
