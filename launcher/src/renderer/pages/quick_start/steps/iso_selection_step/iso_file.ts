/**
 * Files the quick start's ISO step takes, by extension: the disc image itself, plus the formats it
 * explains (compressed images Dolphin's netplay cannot use, and 7z archives). The native "Select"
 * dialog filters by these (by extension, not by MIME type, which differs for .iso between Windows,
 * macOS and Linux); the drop zone accepts the same extensions.
 */
export const ISO_STEP_EXTENSIONS = ["iso", "wbfs", "rvz", "7z"] as const;

export type IsoFileKind = "iso" | "7z" | "compressed";

/** What the ISO step does with a chosen file: verify it ("iso") or explain why it cannot be used. */
export function getIsoFileKind(filePath: string): IsoFileKind {
  const lower = filePath.toLowerCase();
  if (lower.endsWith(".7z")) {
    return "7z";
  }
  if (lower.endsWith(".rvz") || lower.endsWith(".wbfs")) {
    return "compressed";
  }
  return "iso";
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
