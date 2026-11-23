import { cleanup, fireEvent, render, within } from "@testing-library/react";
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

  it("puts the badges for its place before the name, outside the part that is cut short", () => {
    const icon = (c: string) => c.repeat(20);
    const user = {
      ...ann,
      badges: [
        { icon: icon("a"), label: "Dev" },
        { icon: icon("b"), places: ["chat" as const] },
        { icon: icon("c") },
        { icon: icon("d") },
        { icon: icon("e") },
      ],
    };
    // No place, no badges.
    expect(render(<UserName user={user} />).container.querySelector("img")).toBeNull();
    const online = render(<UserName user={user} badgePlace="online" />);
    const imgs = within(online.container).getAllByTestId("name-badge");
    // The chat-only one left out; at most three in a list, in order.
    expect(imgs.map((i) => i.getAttribute("src")?.match(/icons\/(\w+)\.png/)?.[1])).toEqual([
      icon("a"),
      icon("c"),
      icon("d"),
    ]);
    expect(imgs[0].getAttribute("alt")).toBe("Dev");
    // The badges sit beside the name's text box (which clips a long name), never inside it.
    const badges = within(online.container).getByTestId("name-badges");
    const name = within(online.container).getByText("ann");
    expect(badges.contains(name)).toBe(false);
    expect(badges.nextElementSibling?.contains(name)).toBe(true);
    expect(badges.parentElement).toBe(online.container.firstElementChild);
    const chat = render(<UserName user={user} badgePlace="chat" />);
    expect(within(chat.container).getAllByTestId("name-badge").length).toBe(3);
  });
});
