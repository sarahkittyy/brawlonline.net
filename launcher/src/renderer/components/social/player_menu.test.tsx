import type { SocialPlayer, SocialState } from "@social/types";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { useAccount } from "@/lib/hooks/use_account";
import { useSocial } from "@/lib/social/use_social";
import { createServiceProvider } from "@/services";
import type { AdminUser } from "@/services/backend/types";
import type { Services } from "@/services/types";

import { canonicalCode } from "./admin_rename_dialog";
import { PlayerMenu } from "./player_menu";

const ness: SocialPlayer = { uid: "ness", displayName: "Ness", connectCode: "NESS#44" };

function setup(opts: { admin: boolean; user?: Partial<AdminUser> }) {
  let user: AdminUser = {
    uid: "ness",
    displayName: "Ness",
    connectCode: "NESS#44",
    muted: false,
    nameLocked: false,
    banned: false,
    admin: false,
    ...opts.user,
  };
  const backendService = {
    adminUser: vi.fn(async () => user),
    adminSetMuted: vi.fn(async (_uid: string, muted: boolean) => (user = { ...user, muted })),
    adminSetBanned: vi.fn(async (_uid: string, banned: boolean) => (user = { ...user, banned })),
    adminUpdateUser: vi.fn(
      async (_uid: string, u: { displayName?: string; connectCode?: string; nameLocked?: boolean }) => {
        user = { ...user, ...u };
        return user;
      },
    ),
    adminCodeOwner: vi.fn(async (code: string) =>
      code === "LUCA#5" ? { code, uid: "lucas", displayName: "Lucas" } : { code, uid: null, displayName: null },
    ),
  };
  const notificationService = { showError: vi.fn(), showInfo: vi.fn(), showWarning: vi.fn(), showSuccess: vi.fn() };
  const { ServiceProvider } = createServiceProvider({
    services: { backendService, notificationService } as unknown as Services,
  });
  useAccount.setState({ user: { uid: "me" }, userData: { playKey: {}, admin: opts.admin } } as any);
  const state: SocialState = {
    status: "online",
    online: [ness],
    room: null,
    ignored: [],
    invites: [],
    conversations: {},
  };
  useSocial.setState({ state, tabs: [] } as any);
  const onClose = vi.fn();
  const queryClient = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const ui = (target: SocialPlayer | null) => (
    <QueryClientProvider client={queryClient}>
      <ServiceProvider>
        <PlayerMenu target={target ? { player: target, x: 10, y: 10 } : null} onClose={onClose} />
      </ServiceProvider>
    </QueryClientProvider>
  );
  const view = render(ui(ness));
  // The menu closes itself on a click (its parent drops the target); the dialogs stay.
  onClose.mockImplementation(() => view.rerender(ui(null)));
  return { backendService, notificationService, reopen: () => view.rerender(ui(ness)) };
}

afterEach(() => {
  cleanup();
});

describe("PlayerMenu admin tools", () => {
  it("are not there for anyone else", () => {
    const { backendService } = setup({ admin: false });
    expect(screen.getByText("View Profile")).toBeTruthy();
    expect(screen.queryByText("Mute")).toBeNull();
    expect(screen.queryByText("Rename")).toBeNull();
    expect(screen.queryByText("Ban User")).toBeNull();
    expect(backendService.adminUser).not.toHaveBeenCalled();
  });

  it("mute, then offer Unmute", async () => {
    const { backendService, reopen } = setup({ admin: true });
    await waitFor(() => expect(screen.getByTestId("admin-mute").getAttribute("aria-disabled")).toBeNull());
    expect(screen.getByTestId("admin-mute").textContent).toBe("Mute");
    await act(async () => {
      fireEvent.click(screen.getByTestId("admin-mute"));
    });
    expect(backendService.adminSetMuted).toHaveBeenCalledWith("ness", true);
    reopen();
    await waitFor(() => expect(screen.getByTestId("admin-mute").textContent).toBe("Unmute"));
  });

  it("ask before a ban, and ban only on yes", async () => {
    const { backendService } = setup({ admin: true });
    await waitFor(() => expect(screen.getByTestId("admin-ban").getAttribute("aria-disabled")).toBeNull());
    fireEvent.click(screen.getByTestId("admin-ban"));
    expect(screen.getByText("Are you sure?")).toBeTruthy();
    expect(screen.getByText("Ban Ness (NESS#44) for good?")).toBeTruthy();
    fireEvent.click(screen.getByText("Cancel"));
    expect(backendService.adminSetBanned).not.toHaveBeenCalled();
  });

  it("ban after yes", async () => {
    const { backendService } = setup({ admin: true });
    await waitFor(() => expect(screen.getByTestId("admin-ban").getAttribute("aria-disabled")).toBeNull());
    fireEvent.click(screen.getByTestId("admin-ban"));
    await act(async () => {
      fireEvent.click(screen.getByTestId("admin-ban-confirm"));
    });
    expect(backendService.adminSetBanned).toHaveBeenCalledWith("ness", true);
  });

  it("offer neither mute nor ban for another admin", async () => {
    setup({ admin: true, user: { admin: true } });
    await waitFor(() => expect(screen.queryByTestId("admin-mute")).toBeNull());
    expect(screen.queryByTestId("admin-ban")).toBeNull();
    expect(screen.getByTestId("admin-rename-item")).toBeTruthy();
  });

  it("rename: filled in, a taken code can't be applied, the lock goes with Apply", async () => {
    const { backendService } = setup({ admin: true, user: { nameLocked: false } });
    fireEvent.click(screen.getByTestId("admin-rename-item"));
    const name = (await screen.findByTestId("admin-rename-name")) as HTMLInputElement;
    const code = screen.getByTestId("admin-rename-code") as HTMLInputElement;
    await waitFor(() => expect(name.value).toBe("Ness"));
    expect(code.value).toBe("NESS#44");
    const apply = screen.getByTestId("admin-rename-apply") as HTMLButtonElement;
    // Nothing changed yet.
    expect(apply.disabled).toBe(true);

    fireEvent.change(code, { target: { value: "luca#5" } });
    await screen.findByText("Taken by Lucas.");
    expect(apply.disabled).toBe(true);

    fireEvent.change(code, { target: { value: "NESS#7" } });
    fireEvent.change(name, { target: { value: "Ness2" } });
    fireEvent.click(screen.getByTestId("admin-rename-lock"));
    await waitFor(() => expect(apply.disabled).toBe(false));
    await act(async () => {
      fireEvent.click(apply);
    });
    expect(backendService.adminUpdateUser).toHaveBeenCalledWith("ness", {
      displayName: "Ness2",
      connectCode: "NESS#7",
      nameLocked: true,
    });
    await waitFor(() => expect(screen.queryByTestId("admin-rename")).toBeNull());
  });
});

describe("canonicalCode", () => {
  it("reads codes as the server stores them", () => {
    expect(canonicalCode(" abcd#0123 ")).toBe("ABCD#123");
    expect(canonicalCode("A#0")).toBe("A#0");
    for (const bad of ["", "ABCDE#1", "AB#12345", "AB12", "A1#2", "#12"]) {
      expect(canonicalCode(bad)).toBeNull();
    }
  });
});
