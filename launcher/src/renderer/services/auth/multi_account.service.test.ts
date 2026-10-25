import type { AccountsMe } from "@accounts/types";
import { AccountsError } from "@accounts/types";
import { produce } from "immer";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { createMultiAccountService } from "./multi_account.service";

const stored = {
  id: "uid1",
  email: "a@example.com",
  displayName: "Stored Name",
  displayPicture: "",
  lastActive: new Date(0).toISOString(),
};

const record: AccountsMe = {
  uid: "uid1",
  email: "a@example.com",
  emailVerified: true,
  emailVerificationRequired: false,
  displayName: "Server Name",
  connectCode: "ABC#123",
  playKey: "key",
  latestVersion: "",
  role: "user",
  userJson: null,
};

/** A `me` request the test answers (or fails) when it wants to. */
function deferred<T>() {
  let settle!: { resolve: (value: T) => void; reject: (err: unknown) => void };
  const promise = new Promise<T>((resolve, reject) => {
    settle = { resolve, reject };
  });
  return { promise, ...settle };
}

let me: ReturnType<typeof deferred<AccountsMe>>;

beforeEach(() => {
  vi.useFakeTimers();
  me = deferred<AccountsMe>();
  (window as any).electron = {
    settings: {
      getAppSettingsSync: () => ({ accounts: { activeId: "uid1", list: [stored] } }),
      updateSettings: async () => undefined,
    },
    accounts: {
      me: () => me.promise,
      hasSession: async () => true,
    },
  };
});

afterEach(() => {
  vi.useRealTimers();
});

/** init() with the clock moved past the restore wait. */
async function initPastTheWait() {
  const service = createMultiAccountService();
  let done = false;
  const init = service.init().then(() => {
    done = true;
  });
  await vi.advanceTimersByTimeAsync(3999);
  expect(done).toBe(false);
  await vi.advanceTimersByTimeAsync(1);
  await init;
  return service;
}

describe("MultiAccountService start", () => {
  it("uses the server's record when it answers in time", async () => {
    const service = createMultiAccountService();
    const init = service.init();
    me.resolve(record);
    await init;
    expect(service.getActiveUser()?.displayName).toBe("Server Name");
  });

  it("starts with the stored details when the server doesn't answer, then takes the record", async () => {
    const service = await initPastTheWait();
    expect(service.getActiveUser()?.displayName).toBe("Stored Name");
    expect(service.getActiveUser()?.playKey).toBeNull();

    const seen: (AccountsMe | null)[] = [];
    service.onActiveUserChange((user) => seen.push(user));
    me.resolve(record);
    await vi.advanceTimersByTimeAsync(0);
    expect(service.getActiveUser()?.displayName).toBe("Server Name");
    expect(seen.map((u) => u?.displayName)).toEqual(["Server Name"]);
  });

  it("logs out when a late answer says the session expired", async () => {
    const service = await initPastTheWait();
    const seen: (AccountsMe | null)[] = [];
    service.onActiveUserChange((user) => seen.push(user));
    me.reject(new AccountsError("unauthorized", "expired", 401));
    await vi.advanceTimersByTimeAsync(0);
    expect(service.getActiveUser()).toBeNull();
    expect(seen).toEqual([null]);
  });

  it("announces nothing when the late request fails too", async () => {
    const service = await initPastTheWait();
    const seen: (AccountsMe | null)[] = [];
    service.onActiveUserChange((user) => seen.push(user));
    me.reject(new AccountsError("network", "Could not reach the account server"));
    await vi.advanceTimersByTimeAsync(0);
    expect(service.getActiveUser()?.displayName).toBe("Stored Name");
    expect(seen).toEqual([]);
  });
});

describe("MultiAccountService switching", () => {
  it("switches after the account store froze the accounts it was given (bug #140)", async () => {
    const other = { ...stored, id: "uid2", email: "b@example.com", displayName: "Other" };
    (window as any).electron.settings.getAppSettingsSync = () => ({
      accounts: { activeId: "uid1", list: [stored, other] },
    });
    (window as any).electron.accounts.me = async (uid: string) => ({ ...record, uid, email: `${uid}@example.com` });
    const service = createMultiAccountService();
    await service.init();

    // What useAccount does: keep getAccounts()'s list, then an immer update (updateRanking)
    // freezes the whole state, the accounts in it too.
    const state = produce({ accounts: service.getAccounts(), ranked: 0 }, (draft) => {
      draft.ranked = 1;
    });
    expect(Object.isFrozen(state.accounts[0])).toBe(true);

    await service.switchAccount("uid2");
    expect(service.getActiveAccountId()).toBe("uid2");
    expect(service.getAccounts()[0].id).toBe("uid2");
    expect(service.getActiveUser()?.email).toBe("uid2@example.com");
  });
});
