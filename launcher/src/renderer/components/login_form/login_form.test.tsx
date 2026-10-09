import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";

import { LoginForm } from "./login_form";

const signUp = vi.fn();

vi.mock("@/services", () => ({
  useServices: () => ({ authService: { signUp, login: vi.fn(), resetPassword: vi.fn() } }),
}));

const openSignUp = () => {
  render(<LoginForm />);
  fireEvent.click(screen.getByRole("button", { name: "Create an account" }));
};

const typeDisplayName = (value: string) => {
  fireEvent.change(screen.getByLabelText(/Display Name/), { target: { value } });
};

describe("when signing up", () => {
  afterEach(() => {
    // The form keeps its values in a module-level store, so clear the name before unmounting
    typeDisplayName("");
    cleanup();
    signUp.mockReset();
  });

  it("should say the display name is too long before Sign up is clicked", () => {
    openSignUp();
    typeDisplayName("a".repeat(16));

    expect(screen.getByText("Display name is too long")).toBeTruthy();
    expect((screen.getByRole("button", { name: "Sign up" }) as HTMLButtonElement).disabled).toBe(true);
  });

  it("should accept a 15 character display name", () => {
    openSignUp();
    typeDisplayName("a".repeat(15));

    expect(screen.queryByText("Display name is too long")).toBeNull();
    expect((screen.getByRole("button", { name: "Sign up" }) as HTMLButtonElement).disabled).toBe(false);
  });

  it("should not show an error for an empty display name", () => {
    openSignUp();

    expect(screen.queryByText("Display name is too short")).toBeNull();
  });
});
