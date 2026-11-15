/** The queue's words (docs/design/launcher-redesign.md §7, Sarah's own: "ETA: 10+ min", "Searching"). */
export const QueueMessages = {
  etaLong: () => "10+ min",
  searching: (time: string) => `Searching · ${time}`,
};
