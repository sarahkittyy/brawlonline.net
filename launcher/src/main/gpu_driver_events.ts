import { format } from "date-fns";
import { execFile } from "node:child_process";

/**
 * The System log's display driver sources: NVIDIA (nvlddmkm: 13 "Graphics Exception", 153 "Error occurred on
 * GPUID"), AMD (amdkmdag, amdkmdap), Intel (igfx, igfxn) and Windows' own Display 4101 ("Display driver ...
 * stopped responding and has successfully recovered", a TDR). A game whose GPU is reset dies on a lost device
 * (EmissaryRecomp: vkQueueSubmit VK_ERROR_DEVICE_LOST; Dolphin: D3D "device instance has been suspended"), and
 * these say when the driver itself reset (bug report #364).
 */
const DRIVER_SOURCES = ["nvlddmkm", "amdkmdag", "amdkmdap", "igfx", "igfxn", "Display"];

/** How far back the events go, and at most how many. */
const EVENT_DAYS = 14;
const MAX_EVENTS = 200;
const WEVTUTIL_TIMEOUT_MS = 5000;

/** wevtutil's XPath query for the driver events of the last EVENT_DAYS days. */
export function driverEventsQuery(): string {
  const providers = DRIVER_SOURCES.map((name) => `@Name='${name}'`).join(" or ");
  return `*[System[Provider[${providers}] and TimeCreated[timediff(@SystemTime) <= ${EVENT_DAYS * 86400000}]]]`;
}

const unescapeXml = (text: string) =>
  text
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&#(\d+);/g, (_, code: string) => String.fromCharCode(Number(code)))
    .replace(/&amp;/g, "&");

const tag = (xml: string, regex: RegExp) => {
  const m = regex.exec(xml);
  return m ? unescapeXml(m[1]) : null;
};

/**
 * One line per event of `wevtutil qe System /f:RenderedXml` output, oldest first, times in this PC's local time
 * as the launcher's main.log has them: "2026-11-26 22:21:30 nvlddmkm 153 Error: \Device\Video3 | Error occurred
 * on GPUID: 100". The message if Windows renders one, else the event's data strings.
 */
export function driverEventLines(xml: string): string[] {
  const lines: string[] = [];
  for (const event of xml.match(/<Event[\s>][\s\S]*?<\/Event>/g) ?? []) {
    const source = tag(event, /<Provider Name='([^']*)'/) ?? "?";
    const id = tag(event, /<EventID[^>]*>(\d+)<\/EventID>/) ?? "?";
    const created = tag(event, /<TimeCreated SystemTime='([^']*)'/);
    const date = created ? new Date(created) : null;
    const when = date && !Number.isNaN(date.getTime()) ? format(date, "yyyy-MM-dd HH:mm:ss") : created ?? "?";
    const level = tag(event, /<RenderingInfo[\s\S]*?<Level>([^<]*)<\/Level>/) ?? `level ${tag(event, /<Level>(\d+)/)}`;
    const message = tag(event, /<Message>([\s\S]*?)<\/Message>/)?.trim();
    const data = [...event.matchAll(/<Data(?: [^>]*)?>([\s\S]*?)<\/Data>/g)]
      .map((m) => unescapeXml(m[1]).trim())
      .filter((d) => d !== "");
    const text = message || data.join(" | ");
    lines.push(`${when} ${source} ${id} ${level}${text ? `: ${text.replace(/\s+/g, " ")}` : ""}`);
  }
  // wevtutil reads newest first (/rd:true, so /c keeps the newest); the logs read oldest first.
  return lines.reverse();
}

/** The text of gpu-driver-events.txt in a logs zip; never rejects. */
export function driverEventsText(): Promise<string> {
  const header =
    `Display driver events in the Windows System log, the last ${EVENT_DAYS} days (local time). ` +
    `Display 4101 = the driver stopped responding and Windows reset it (TDR); ` +
    `nvlddmkm 13/153, amdkmdag, igfx = the driver's own errors.\n\n`;
  return new Promise((resolve) => {
    execFile(
      "wevtutil",
      ["qe", "System", `/q:${driverEventsQuery()}`, "/f:RenderedXml", `/c:${MAX_EVENTS}`, "/rd:true"],
      { timeout: WEVTUTIL_TIMEOUT_MS, windowsHide: true, maxBuffer: 16 * 1024 * 1024 },
      (err, stdout) => {
        if (err) {
          resolve(`${header}wevtutil failed: ${err.message}\n`);
          return;
        }
        const lines = driverEventLines(stdout);
        resolve(`${header}${lines.length > 0 ? lines.join("\n") : "none"}\n`);
      },
    );
  });
}
