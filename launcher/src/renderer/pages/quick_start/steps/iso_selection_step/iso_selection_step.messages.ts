export const IsoSelectionStepMessages = {
  selectMeleeIso: () => "Select Brawl ISO",
  thisApplicationUsesNtsc: () =>
    "This application uses an NTSC-U Super Smash Bros. Brawl game backup (revision 1 or 2).",
  verifyingIso: () => "Verifying ISO...",
  orDragAndDropHere: () => "or drag and drop here",
  providedIsoWillNotWork: (productName: string) =>
    "Provided ISO will not work with {0}. Please provide an unmodified NTSC-U Brawl ISO (revision 1 or 2).",
  sevenZFilesMustBeUncompressed: () => "7z files must be uncompressed to be used in Dolphin.",
  rvzFilesAreIncompatible: (productName: string) =>
    "Compressed disc images (RVZ, WBFS) are not supported by {0}. Please provide an uncompressed ISO.",
  select: () => "Select",
};
