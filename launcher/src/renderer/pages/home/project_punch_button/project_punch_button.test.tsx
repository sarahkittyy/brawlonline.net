import { render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { ProjectPunchButtonView } from "./project_punch_button";

describe("ProjectPunchButtonView", () => {
  it("mounts the progress bar only while a job is running", () => {
    const onClick = vi.fn();
    const { getByRole, queryByTestId, rerender } = render(<ProjectPunchButtonView job={null} onClick={onClick} />);
    const job = { kind: "install", phase: "download", current: 0, total: 0 } as const;

    expect(queryByTestId("project-punch-progress")).toBeNull();

    rerender(<ProjectPunchButtonView job={job} onClick={onClick} />);
    expect(getByRole("progressbar").hasAttribute("aria-valuenow")).toBe(false);

    rerender(<ProjectPunchButtonView job={{ ...job, current: 25, total: 100 }} onClick={onClick} />);
    expect(getByRole("progressbar").getAttribute("aria-valuenow")).toBe("25");

    rerender(<ProjectPunchButtonView job={null} onClick={onClick} />);
    expect(queryByTestId("project-punch-progress")).toBeNull();
  });
});
