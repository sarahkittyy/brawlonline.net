import type { AccountsMe } from "@accounts/types";

import { generateDisplayPicture } from "@/lib/display_picture";

import type { AuthUser } from "./types";

export function mapAccountToAuthUser(user: AccountsMe): AuthUser {
  return {
    uid: user.uid,
    displayName: user.displayName || "",
    displayPicture: generateDisplayPicture(user.uid),
    email: user.email || "",
    // A server that does not require verification never blocks on it (backend-design.md section 3).
    emailVerified: user.emailVerified || !user.emailVerificationRequired,
  };
}
