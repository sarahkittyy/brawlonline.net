import { describe, expect, it } from "vitest";

import { formatRelativeDate } from "./time";

const NOW = new Date("2026-08-06T20:00:00-04:00");
const HOUR = 60 * 60 * 1000;
const DAY = 24 * HOUR;

function dateAfter(ms: number): Date {
  return new Date(NOW.getTime() + ms);
}

describe("formatRelativeDate", () => {
  describe("future events", () => {
    it("should say tomorrow for events exactly 24 hours away", () => {
      expect(formatRelativeDate(dateAfter(DAY), "en-US", NOW)).toBe("tomorrow");
    });

    it("should say in 24 hours for events just under a day away", () => {
      expect(formatRelativeDate(dateAfter(DAY - HOUR), "en-US", NOW)).toBe("in 23 hours");
    });

    it("should say in 2 days for events exactly 25 hours away", () => {
      expect(formatRelativeDate(dateAfter(25 * HOUR), "en-US", NOW)).toBe("in 2 days");
    });

    it("should say in 2 days for events 33 hours away", () => {
      expect(formatRelativeDate(dateAfter(33 * HOUR), "en-US", NOW)).toBe("in 2 days");
    });

    it("should say in 2 days for events 47 hours away", () => {
      expect(formatRelativeDate(dateAfter(47 * HOUR), "en-US", NOW)).toBe("in 2 days");
    });

    it("should say in 2 days for events exactly 2 days away", () => {
      expect(formatRelativeDate(dateAfter(2 * DAY), "en-US", NOW)).toBe("in 2 days");
    });

    it("should say in 3 days for events just over 2 days away", () => {
      expect(formatRelativeDate(dateAfter(2 * DAY + HOUR), "en-US", NOW)).toBe("in 3 days");
    });

    it("should say in 13 days for events 13 days away", () => {
      expect(formatRelativeDate(dateAfter(13 * DAY), "en-US", NOW)).toBe("in 13 days");
    });

    it("should say in 2 weeks for events exactly 14 days away", () => {
      expect(formatRelativeDate(dateAfter(14 * DAY), "en-US", NOW)).toBe("in 2 weeks");
    });

    it("should say in 3 weeks for events 20 days away", () => {
      expect(formatRelativeDate(dateAfter(20 * DAY), "en-US", NOW)).toBe("in 3 weeks");
    });

    it("should say in 4 weeks for events 25 days away", () => {
      expect(formatRelativeDate(dateAfter(25 * DAY), "en-US", NOW)).toBe("in 4 weeks");
    });

    it("should say next month for events 30 days away", () => {
      expect(formatRelativeDate(dateAfter(30 * DAY), "en-US", NOW)).toBe("next month");
    });

    it("should say in 9 weeks for events 60 days away", () => {
      expect(formatRelativeDate(dateAfter(60 * DAY), "en-US", NOW)).toBe("in 9 weeks");
    });

    it("should say in 3 months for events 92 days away", () => {
      expect(formatRelativeDate(dateAfter(92 * DAY), "en-US", NOW)).toBe("in 3 months");
    });

    it("should say next year for events 365 days away", () => {
      expect(formatRelativeDate(dateAfter(365 * DAY), "en-US", NOW)).toBe("next year");
    });
  });

  describe("past events", () => {
    it("should say yesterday for events 25 hours ago", () => {
      expect(formatRelativeDate(dateAfter(-25 * HOUR), "en-US", NOW)).toBe("yesterday");
    });

    it("should say 2 days ago for events 47 hours ago", () => {
      expect(formatRelativeDate(dateAfter(-47 * HOUR), "en-US", NOW)).toBe("2 days ago");
    });
  });

  describe("invalid input", () => {
    it("should throw on an invalid date", () => {
      expect(() => formatRelativeDate(new Date("invalid"))).toThrow("Invalid targetDate");
    });
  });
});
