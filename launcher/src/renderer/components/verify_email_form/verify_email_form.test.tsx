import { act, cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import type { AuthUser } from "@/services/auth/types";

import { VerifyEmailForm } from "./verify_email_form";

const changeEmail = vi.fn();
const showError = vi.fn();

vi.mock("@/services", () => ({
  useServices: () => ({
    authService: { changeEmail, sendVerificationEmail: vi.fn(), refreshUser: vi.fn(), getCurrentUser: vi.fn() },
    notificationService: { showError },
  }),
}));

const user = (emailVerified: boolean): AuthUser => ({
  uid: "uid-1",
  displayName: "Graham",
  displayPicture: "",
  email: "graham@typo.test",
  emailVerified,
});

const openChangeEmail = () => {
  render(<VerifyEmailForm user={user(false)} />);
  fireEvent.click(screen.getByText("Change email"));
  fireEvent.change(screen.getByLabelText(/New email/), { target: { value: "graham@right.test" } });
};

describe("a wrong email on the verify step", () => {
  afterEach(() => {
    cleanup();
    changeEmail.mockReset();
    showError.mockReset();
  });

  it("can be changed before it is verified", async () => {
    openChangeEmail();
    changeEmail.mockResolvedValue(undefined);
    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Change" }));
    });

    expect(changeEmail).toHaveBeenCalledWith("graham@right.test");
    expect(screen.queryByLabelText(/New email/)).toBeNull();
    expect(screen.getByText("Wrong email?")).toBeTruthy();
  });

  it("keeps the field open and shows the server's error when refused", async () => {
    openChangeEmail();
    changeEmail.mockRejectedValue(new Error("An account with this email already exists"));
    await act(async () => {
      fireEvent.click(screen.getByRole("button", { name: "Change" }));
    });

    expect(showError).toHaveBeenCalledWith("An account with this email already exists");
    expect(screen.getByLabelText(/New email/)).toBeTruthy();
  });

  it("is not offered once the email is verified", () => {
    render(<VerifyEmailForm user={user(true)} />);

    expect(screen.queryByText("Wrong email?")).toBeNull();
  });
});
