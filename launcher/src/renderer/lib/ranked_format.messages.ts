export const RankedFormatMessages = {
  ranked: () => "Ranked",
  unranked: () => "Unranked",
  direct: () => "Direct",
  unknownPlayer: () => "Unknown player",
  inProgress: () => "In progress",
  abandonedBy: (name: string) => `Abandoned by ${name}`,
  abandoned: () => "Abandoned",
  voidConnection: () => "Void: the connection broke, no rating change",
  voidDisagreement: () => "Void: the reports disagree, no rating change",
  voidNoResult: () => "Void: no result was reported, no rating change",
  voidOther: () => "Void: no rating change",
};
