import { format } from "date-fns";
import { describe, expect, it } from "vitest";

import { driverEventLines, driverEventsQuery } from "./gpu_driver_events";

// As `wevtutil qe System /f:RenderedXml /rd:true` prints them: newest first, on one line.
const NVLDDMKM_153 =
  "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='nvlddmkm'/>" +
  "<EventID Qualifiers='0'>153</EventID><Version>0</Version><Level>2</Level><Task>0</Task><Opcode>0</Opcode>" +
  "<TimeCreated SystemTime='2026-11-26T13:25:11.8396241Z'/><Channel>System</Channel><Computer>pc</Computer>" +
  "</System><EventData><Data>\\Device\\00000430</Data><Data>Error occurred on GPUID: 100</Data>" +
  "<Binary>0000</Binary></EventData><RenderingInfo Culture='en-US'><Level>Error</Level><Opcode>Info</Opcode>" +
  "</RenderingInfo></Event>";
const DISPLAY_4101 =
  "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Display'/>" +
  "<EventID Qualifiers='0'>4101</EventID><Level>3</Level><TimeCreated SystemTime='2026-11-26T13:24:39.6223384Z'/>" +
  "</System><EventData><Data>nvlddmkm</Data><Data></Data></EventData><RenderingInfo Culture='en-US'>" +
  "<Message>Display driver nvlddmkm stopped responding and has successfully recovered.</Message>" +
  "<Level>Warning</Level></RenderingInfo></Event>";
const NVLDDMKM_13 =
  "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='nvlddmkm'/>" +
  "<EventID Qualifiers='49322'>13</EventID><Level>2</Level><TimeCreated SystemTime='2026-11-26T12:31:19.8446503Z'/>" +
  "</System><EventData><Data>\\Device\\00000430</Data><Data>Graphics Exception: ESR 0x407020=0x101480 &amp; 0x0</Data>" +
  "</EventData><RenderingInfo Culture='en-US'><Level>Error</Level></RenderingInfo></Event>";

const local = (iso: string) => format(new Date(iso), "yyyy-MM-dd HH:mm:ss");

describe("driverEventLines", () => {
  it("writes one line per event, oldest first, with the message or else the data", () => {
    expect(driverEventLines(`${NVLDDMKM_153}${DISPLAY_4101}\r\n${NVLDDMKM_13}\r\n`)).toEqual([
      `${local(
        "2026-11-26T12:31:19.8446503Z",
      )} nvlddmkm 13 Error: \\Device\\00000430 | Graphics Exception: ESR 0x407020=0x101480 & 0x0`,
      `${local(
        "2026-11-26T13:24:39.6223384Z",
      )} Display 4101 Warning: Display driver nvlddmkm stopped responding and has successfully recovered.`,
      `${local("2026-11-26T13:25:11.8396241Z")} nvlddmkm 153 Error: \\Device\\00000430 | Error occurred on GPUID: 100`,
    ]);
  });

  it("is empty when there are none", () => {
    expect(driverEventLines("")).toEqual([]);
  });
});

describe("driverEventsQuery", () => {
  it("asks for the display drivers' sources in the last 14 days", () => {
    const query = driverEventsQuery();
    expect(query).toContain("@Name='nvlddmkm' or ");
    expect(query).toContain("@Name='Display'");
    expect(query).toContain("timediff(@SystemTime) <= 1209600000");
  });
});
