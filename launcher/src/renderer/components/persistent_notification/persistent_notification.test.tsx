import { defaultServiceUrls, MAC_DOWNLOAD_URL } from "@common/product";
import { fireEvent, render } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { useAppStore } from "@/lib/hooks/use_app_store";

import { PersistentNotification } from "./persistent_notification";

// The store reads window.electron when it is imported, so the stub is in place before the imports run.
const { openExternal, installAppUpdate, bootstrap } = vi.hoisted(() => {
  const stub = {
    openExternal: vi.fn(async (_url: string) => undefined),
    installAppUpdate: vi.fn(async () => ({ success: true })),
    bootstrap: { locale: "en-US", launcherUpdateMode: "install" as "install" | "download" },
  };
  (window as any).electron = {
    bootstrap: stub.bootstrap,
    shell: { openExternal: stub.openExternal },
  };
  return stub;
});

vi.mock("@/lib/hooks/use_app_update", () => ({ useAppUpdate: () => ({ installAppUpdate }) }));

function stubElectron(launcherUpdateMode: "install" | "download") {
  bootstrap.launcherUpdateMode = launcherUpdateMode;
}

describe("PersistentNotification", () => {
  beforeEach(() => {
    openExternal.mockClear();
    installAppUpdate.mockClear();
    useAppStore.setState({
      updateVersion: "",
      updateReady: false,
      updateDownloadProgress: 0,
      updateDownloadFailed: false,
    });
  });

  it("offers the download once an update is found, on macOS while it cannot update itself", () => {
    stubElectron("download");
    const { container } = render(<PersistentNotification />);
    expect(container.textContent).toBe("");

    useAppStore.setState({ updateVersion: "0.1.30" });
    const { container: found } = render(<PersistentNotification />);
    // (Message arguments are filled in by i18next at run time, not in tests.)
    expect(found.textContent).toContain("is now available!");
    expect(found.querySelector("button")!.textContent).toMatch(/^Download /);
    expect(found.textContent).not.toContain("Install update");

    fireEvent.click(found.querySelector("button")!);
    expect(openExternal).toHaveBeenCalledWith(MAC_DOWNLOAD_URL);
    expect(installAppUpdate).not.toHaveBeenCalled();
  });

  it("keeps Slippi's install-and-restart bar elsewhere", () => {
    stubElectron("install");
    const { container: none } = render(<PersistentNotification />);
    expect(none.textContent).toBe("");

    // Found: shown at once (no Dolphin starts until it is installed), before any download progress.
    useAppStore.setState({ updateVersion: "0.1.30" });
    const { container } = render(<PersistentNotification />);
    expect(container.textContent).toMatch(/^Downloading version/);

    useAppStore.setState({ updateReady: true });
    const { container: ready } = render(<PersistentNotification />);
    expect(ready.textContent).toContain("is now available!");
    expect(ready.querySelector("button")!.textContent).toBe("Install update");
    expect(ready.textContent).not.toContain("Download");
  });

  it("offers the website's download when downloading the update failed", () => {
    stubElectron("install");
    useAppStore.setState({ updateVersion: "0.1.30", updateDownloadFailed: true });
    const { container } = render(<PersistentNotification />);
    expect(container.textContent).toContain("is now available!");
    expect(container.querySelector("button")!.textContent).toBe("Download manually");

    fireEvent.click(container.querySelector("button")!);
    expect(openExternal).toHaveBeenCalledWith(defaultServiceUrls.launcherUpdates);
    expect(installAppUpdate).not.toHaveBeenCalled();
  });
});
