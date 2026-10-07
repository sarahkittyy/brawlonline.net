import type { DolphinService } from "@dolphin/types";
import type { ReplayService } from "@replays/types";

import type { AuthService } from "./auth/types";
import type { BackendService } from "./backend/types";
import type { I18nService } from "./i18n/types";
import type { NotificationService } from "./notification/types";

export type Services = {
  authService: AuthService;
  backendService: BackendService;
  dolphinService: DolphinService;
  replayService: ReplayService;
  notificationService: NotificationService;
  i18nService: I18nService;
};
