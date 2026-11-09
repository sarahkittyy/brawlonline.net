import { cleanup, fireEvent, render } from "@testing-library/react";
import { afterEach, describe, expect, it } from "vitest";

import { useProfileView } from "@/lib/hooks/use_profile_view";

import { UserName } from "./user_name";

const ann = { uid: "u1", displayName: "ann", connectCode: "ANN#1" };

describe("UserName", () => {
  afterEach(() => {
    cleanup();
    useProfileView.setState({ target: null });
  });

  it("shows the name and the code after it, the code only where asked", () => {
    expect(render(<UserName user={ann} />).container.textContent).toBe("annANN#1");
    expect(render(<UserName user={ann} showCode={false} />).container.textContent).toBe("ann");
    expect(render(<UserName user={{ displayName: "bob" }} />).container.textContent).toBe("bob");
  });

  it("puts the rank's badge before the name, and a badge-wide gap without a rank", () => {
    const gold = render(<UserName user={{ ...ann, rank: { tier: "gold", division: 2 } }} showBadge={true} />);
    expect(gold.getByAltText("Gold 2")).toBeTruthy();
    const none = render(<UserName user={{ ...ann, rank: null }} showBadge={true} />);
    expect(none.container.querySelector("img")).toBeNull();
    expect(none.container.firstElementChild?.childElementCount).toBe(2);
    expect(
      render(<UserName user={{ ...ann, rank: { tier: "gold", division: 2 } }} />).container.querySelector("img"),
    ).toBeNull();
  });

  it("opens the player's profile from the name when it's a link", () => {
    const plain = render(<UserName user={ann} />);
    expect(plain.container.querySelector("button")).toBeNull();
    const { container } = render(<UserName user={ann} profileLink={true} />);
    fireEvent.click(container.querySelector("[data-profile-link=u1]")!);
    expect(useProfileView.getState().target).toEqual({ uid: "u1", displayName: "ann", connectCode: "ANN#1" });
  });
});
