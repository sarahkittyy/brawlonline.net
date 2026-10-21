/**
 * Disc images the launcher takes for Brawl, by extension: every format Dolphin reads a Wii disc
 * from (the check reads the game's data, not the file, docs/design/disc-images.md). Settings >
 * Game's dialog filters by these too (by extension, not by MIME type, which differs for .iso
 * between Windows, macOS and Linux).
 */
export const DISC_IMAGE_EXTENSIONS = ["iso", "rvz", "wbfs", "wia", "gcz", "ciso"] as const;

/** The quick start's ISO step also takes 7z archives, to explain them. */
export const ISO_STEP_EXTENSIONS = [...DISC_IMAGE_EXTENSIONS, "7z"] as const;

export type IsoFileKind = "iso" | "7z";

/** What the ISO step does with a chosen file: verify it ("iso") or explain why it cannot be used. */
export function getIsoFileKind(filePath: string): IsoFileKind {
  return filePath.toLowerCase().endsWith(".7z") ? "7z" : "iso";
}

type FileIdentity = Pick<File, "name" | "size" | "lastModified">;

/**
 * The native file object (from the drop event itself) that matches the file react-dropzone accepted.
 * Only a native file has a path on disk (`webUtils.getPathForFile`).
 */
export function findNativeFile<T extends FileIdentity>(
  accepted: FileIdentity,
  nativeFiles: readonly T[] | null | undefined,
): T | undefined {
  return nativeFiles?.find(
    (f) => f.name === accepted.name && f.size === accepted.size && f.lastModified === accepted.lastModified,
  );
}
