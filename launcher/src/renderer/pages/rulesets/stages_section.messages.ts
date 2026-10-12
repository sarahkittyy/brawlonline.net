export const StagesMessages = {
  starter: () => "Starter",
  counterpick: () => "Counterpick",
  off: () => "Off",
  otherStages: () => "Other stages",
  otherOn: (n: number) => (n === 0 ? "All off" : `${n} on`),
};
