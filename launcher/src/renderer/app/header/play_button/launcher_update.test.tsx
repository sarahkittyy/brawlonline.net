import { launcherDownloadUrl, MAC_DOWNLOAD_URL } from "@common/product";
import { act, fireEvent, render } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { useAppStore } from "@/lib/hooks/use_app_store";

import { LauncherUpdate } from "./launcher_update";

// The store reads window.electron when it is imported, so the stub is in place before the imports run.
const { openExternal, installAppUpdate, showError, bootstrap } = vi.hoisted(() => {
  const stub = {
    openExternal: vi.fn(async (_url: string) => undefined),
    installAppUpdate: vi.fn(async (): Promise<{ success: boolean; error?: string }> => ({ success: true })),
    showError: vi.fn(),
    bootstrap: {
      locale: "en-US",
      launcherUpdateMode: "install" as "install" | "download",
      isMac: false,
      isLinux: false,
    },
  };
  (window as any).electron = {
    bootstrap: stub.bootstrap,
    shell: { openExternal: stub.openExternal },
  };
  return stub;
});

vi.mock("@/lib/hooks/use_app_update", () => ({ useAppUpdate: () => ({ installAppUpdate }) }));
vi.mock("@/lib/hooks/use_toasts", () => ({ useToasts: () => ({ showError }) }));
vi.stubGlobal("__VERSION__", "0.1.98");

const button = (container: HTMLElement) => container.querySelector("button")!;
const fill = (container: HTMLElement) => container.querySelector('[data-testid="play-button-fill"]');

describe("LauncherUpdate", () => {
  beforeEach(() => {
    openExternal.mockClear();
    installAppUpdate.mockClear();
    showError.mockClear();
    bootstrap.launcherUpdateMode = "install";
    useAppStore.setState({
      updateVersion: "",
      updateReady: false,
      updateDownloadProgress: 0,
      updateDownloadFailed: false,
    });
  });

  it("shows nothing while no update is out", () => {
    const { container } = render(<LauncherUpdate />);
    expect(container.textContent).toBe("");
  });

  it("fills as the update downloads, then installs it", async () => {
    // Found: shown at once (no Dolphin starts until it is installed), before any download progress.
    useAppStore.setState({ updateVersion: "0.1.99" });
    const { container } = render(<LauncherUpdate />);
    expect(container.textContent).toBe("Update 0%v0.1.98 → v0.1.99");
    expect(button(container).disabled).toBe(true);
    expect(fill(container)).not.toBeNull();

    act(() => useAppStore.setState({ updateDownloadProgress: 42.7 }));
    expect(container.textContent).toBe("Update 42%v0.1.98 → v0.1.99");

    act(() => useAppStore.setState({ updateReady: true }));
    expect(container.textContent).toBe("Updatev0.1.98 → v0.1.99");
    expect(button(container).disabled).toBe(false);
    expect(fill(container)).toBeNull();

    await act(async () => fireEvent.click(button(container)));
    expect(installAppUpdate).toHaveBeenCalledTimes(1);
    expect(openExternal).not.toHaveBeenCalled();
    // The launcher restarts: the button stays disabled until it does.
    expect(button(container).disabled).toBe(true);
  });

  it("opens the website's installer when the download or the install failed", async () => {
    useAppStore.setState({ updateVersion: "0.1.99", updateDownloadProgress: 10, updateDownloadFailed: true });
    const { container } = render(<LauncherUpdate />);
    expect(container.textContent).toBe("Updatev0.1.98 → v0.1.99");
    fireEvent.click(button(container));
    expect(openExternal).toHaveBeenCalledWith(launcherDownloadUrl("win32"));
    expect(installAppUpdate).not.toHaveBeenCalled();

    openExternal.mockClear();
    installAppUpdate.mockResolvedValueOnce({ success: false, error: "timed out" });
    act(() => useAppStore.setState({ updateDownloadFailed: false, updateReady: true }));
    await act(async () => fireEvent.click(button(container)));
    expect(installAppUpdate).toHaveBeenCalledTimes(1);
    expect(showError).toHaveBeenCalledTimes(1);
    expect(button(container).disabled).toBe(false);
    fireEvent.click(button(container));
    expect(openExternal).toHaveBeenCalledWith(launcherDownloadUrl("win32"));
    expect(installAppUpdate).toHaveBeenCalledTimes(1);
  });

  it("offers the website's download on macOS while it cannot update itself", () => {
    bootstrap.launcherUpdateMode = "download";
    useAppStore.setState({ updateVersion: "0.1.99" });
    const { container } = render(<LauncherUpdate />);
    expect(container.textContent).toBe("Updatev0.1.98 → v0.1.99");
    expect(button(container).disabled).toBe(false);
    fireEvent.click(button(container));
    expect(openExternal).toHaveBeenCalledWith(MAC_DOWNLOAD_URL);
    expect(installAppUpdate).not.toHaveBeenCalled();
  });
});
