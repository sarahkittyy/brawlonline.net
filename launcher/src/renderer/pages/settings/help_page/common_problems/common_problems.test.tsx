import { render } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { CommonProblems } from "./common_problems";

describe("CommonProblems", () => {
  it("says what to install when Dolphin's DLLs are missing", () => {
    const { getByText } = render(<CommonProblems />);
    expect(getByText('"Required DLLs for launching Dolphin are missing"')).toBeTruthy();
    const link = getByText("vc_redist.x64.exe").closest("a");
    expect(link?.getAttribute("href")).toBe("https://aka.ms/vc14/vc_redist.x64.exe");
    expect(link?.getAttribute("target")).toBe("_blank");
    expect(getByText(/Media Feature Pack/)).toBeTruthy();
  });
});
