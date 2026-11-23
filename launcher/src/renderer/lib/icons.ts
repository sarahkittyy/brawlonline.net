import { iconUrl, isIconId } from "@common/badges";
import { defaultServiceUrls } from "@common/product";
import { create } from "zustand";

/**
 * Where badge and main icons come from: the accounts server this launcher talks to (PPO_ACCOUNTS_URL for staging and
 * local servers), read from the main process once; the production one until then. Stories set it with `setIconBase`.
 */
const useIconBase = create<{ base: string }>(() => ({ base: defaultServiceUrls.accountsApi }));

let asked = false;
function askBase() {
  if (asked) {
    return;
  }
  asked = true;
  const accounts = (window as any).electron?.accounts;
  if (typeof accounts?.getServiceUrls !== "function") {
    return;
  }
  void Promise.resolve(accounts.getServiceUrls())
    .then((urls: { accountsApi?: string } | undefined) => {
      if (urls?.accountsApi) {
        useIconBase.setState({ base: urls.accountsApi.replace(/\/+$/, "") });
      }
    })
    .catch(() => undefined);
}

/** The accounts server icons come from (for `iconUrl`). */
export function useIconBaseUrl(): string {
  askBase();
  return useIconBase((s) => s.base);
}

/** The address of an icon (an icon id from the server); undefined without one. */
export function useIconUrl(id: string | null | undefined): string | undefined {
  const base = useIconBaseUrl();
  return isIconId(id) ? iconUrl(base, id) : undefined;
}

export function setIconBase(base: string) {
  asked = true;
  useIconBase.setState({ base: base.replace(/\/+$/, "") });
}
