export const RoomNameMessages = {
  roomName: () => "Room name",
  rename: () => "Name your room",
  renameTooltip: () => "Name your room. Your next rooms get this name too.",
  count: (n: number, max: number) => `${n}/${max}`,
  /** The name of a room its host didn't name. */
  defaultName: (host: string) => `${host}'s room`,
};
