export const ItemsMessages = {
  items: () => "Items",
  frequency: () => "Frequency",
  allOn: () => "All on",
  allOff: () => "All off",
  settings: () => "P+ item settings",
  on: () => "On",
  off: () => "Off",
  allItems: (frequency: string) => `All items · ${frequency}`,
  someItems: (n: number, frequency: string) => `${n} ${n === 1 ? "item" : "items"} · ${frequency}`,
  noneOn: () => "No item is on, so no items appear (as in P+).",
  bombRain: () => "Bomb Rain: Bob-ombs drop from the sky as in sudden death (P+'s).",
  frequencyNone: () => "Frequency is None, so no items appear (as in P+).",
};
