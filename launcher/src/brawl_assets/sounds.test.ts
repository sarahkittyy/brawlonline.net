import fs from "fs";
import os from "os";
import path from "path";
import { afterAll, describe, expect, it } from "vitest";

import { BRSAR_FILE, SOUND_ROLES } from "./catalog";
import { extractMissingSounds, readManifest, runExtraction, soundsOutdated } from "./extract";
import { buildBrsar } from "./formats/brsar";
import { buildRwsd, WaveEncoding } from "./formats/rwsd";
import { buildSawnd } from "./formats/sawnd";
import type { GameFileSource } from "./sources";
import { DiscFolderSource } from "./sources";
import { decodeWav } from "./wav";

// Synthetic sound archives only: PCM16 ramps under the catalog's symbol names, no game audio.

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), "brawl-sounds-test-"));
afterAll(() => fs.rmSync(tmpRoot, { recursive: true, force: true }));

const soundName = (role: string) => SOUND_ROLES.find((r) => r.role === role)!.candidates[0].sound;

function pcmRwsd(samples: number[], sampleRate = 32000) {
  const data = Buffer.alloc(samples.length * 2);
  samples.forEach((s, i) => data.writeInt16BE(s, i * 2));
  return buildRwsd(
    [{ waveIndex: 0 }],
    [{ encoding: WaveEncoding.PCM16, sampleRate, sampleCount: samples.length, channels: [{ data }] }],
  );
}

const ramp = (n: number, step: number) => Array.from({ length: n }, (_, i) => i * step);

/** Group 0 holds the invite sound, group 1 the chat sound (as the disc's joucyu and Fox groups do). */
function makeArchive() {
  const common = pcmRwsd(ramp(3200, 3)); // 100 ms
  const fox = pcmRwsd(ramp(640, -5)); // 20 ms
  return buildBrsar(
    [
      { name: "snd_se_common_unrelated", fileId: 0, index: 0 },
      { name: soundName("invite"), fileId: 0, index: 0 },
      { name: soundName("chat-message"), fileId: 1, index: 0 },
    ],
    [
      { name: "snd_group_joucyu", files: [{ fileId: 0, ...common }] },
      { name: "snd_group_chara_Fox", files: [{ fileId: 1, ...fox }] },
    ],
  );
}

function makeDisc(root: string) {
  const file = path.join(root, "DATA", "files", ...BRSAR_FILE.split("/"));
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.mkdirSync(path.join(root, "DATA", "files", "system"), { recursive: true });
  fs.writeFileSync(file, makeArchive());
  return root;
}

const fakeSd = (files: Record<string, Buffer>): GameFileSource => ({
  kind: "sd",
  label: "fake-sd",
  read: async (p) => files[p] ?? null,
  close: async () => undefined,
});

describe("sound extraction (synthetic archive)", () => {
  it("writes sounds/<role>.wav from the disc and records them in the manifest", async () => {
    const disc = await DiscFolderSource.open(makeDisc(path.join(tmpRoot, "disc")));
    const cacheDir = path.join(tmpRoot, "cache");
    const manifest = await runExtraction(disc, null, { cacheDir });
    expect(manifest.sounds).toEqual({
      "chat-message": {
        file: "sounds/chat-message.wav",
        origin: "disc",
        durationMs: 20,
        sound: soundName("chat-message"),
        soundId: 2,
        source: BRSAR_FILE,
      },
      invite: {
        file: "sounds/invite.wav",
        origin: "disc",
        durationMs: 100,
        sound: soundName("invite"),
        soundId: 1,
        source: BRSAR_FILE,
      },
    });
    const wav = decodeWav(fs.readFileSync(path.join(cacheDir, "sounds", "invite.wav")));
    expect(wav.sampleRate).toBe(32000);
    expect(Array.from(wav.channels[0].subarray(0, 4))).toEqual([0, 3, 6, 9]);
    expect(manifest.missing.filter((m) => m.startsWith("sound:"))).toEqual([]);
  });

  it("takes a sound from P+'s group replacement on the SD card, else from the disc", async () => {
    const disc = await DiscFolderSource.open(makeDisc(path.join(tmpRoot, "disc2")));
    const replaced = pcmRwsd(ramp(1600, 7), 16000);
    const sd = fakeSd({ "sfx/000.sawnd": buildSawnd(0, [{ fileId: 0, ...replaced }]) });
    const cacheDir = path.join(tmpRoot, "cache2");
    const manifest = await runExtraction(disc, sd, { cacheDir });
    expect(manifest.sounds?.invite).toMatchObject({ origin: "sd", source: "sfx/000.sawnd", durationMs: 100 });
    expect(manifest.sounds?.["chat-message"]).toMatchObject({ origin: "disc", source: BRSAR_FILE });
    const wav = decodeWav(fs.readFileSync(path.join(cacheDir, "sounds", "invite.wav")));
    expect(wav.sampleRate).toBe(16000);
    expect(Array.from(wav.channels[0].subarray(0, 3))).toEqual([0, 7, 14]);
  });

  it("falls back to the disc with a warning when the SD replacement is broken", async () => {
    const disc = await DiscFolderSource.open(makeDisc(path.join(tmpRoot, "disc3")));
    const sd = fakeSd({ "sfx/000.sawnd": Buffer.from([2, 0, 0, 0, 7, 0, 0, 0, 0]) });
    const manifest = await runExtraction(disc, sd, { cacheDir: path.join(tmpRoot, "cache3") });
    expect(manifest.sounds?.invite?.origin).toBe("disc");
    expect(manifest.warnings.some((w) => w.startsWith("sound invite (sd:"))).toBe(true);
  });

  it("records missing sounds without failing the extraction", async () => {
    const root = path.join(tmpRoot, "nodisc");
    fs.mkdirSync(path.join(root, "system"), { recursive: true });
    const manifest = await runExtraction(await DiscFolderSource.open(root), null, {
      cacheDir: path.join(tmpRoot, "c4"),
    });
    expect(manifest.sounds).toEqual({});
    expect(manifest.missing).toEqual(expect.arrayContaining(["sound:chat-message", "sound:invite"]));
  });

  it("fills the sounds into a cache extracted before they existed", async () => {
    const discRoot = makeDisc(path.join(tmpRoot, "disc5"));
    const cacheDir = path.join(tmpRoot, "cache5");
    expect(await extractMissingSounds({ disc: { kind: "folder", path: discRoot }, cacheDir })).toBeNull();

    const full = await runExtraction(await DiscFolderSource.open(discRoot), null, { cacheDir });
    const old = { ...full, extractedAt: "2026-10-01T00:00:00.000Z" } as Partial<typeof full>;
    delete old.sounds;
    fs.rmSync(path.join(cacheDir, "sounds"), { recursive: true });
    fs.writeFileSync(path.join(cacheDir, "manifest.json"), JSON.stringify(old));

    const filled = await extractMissingSounds({ disc: { kind: "folder", path: discRoot }, cacheDir });
    expect(filled?.extractedAt).toBe("2026-10-01T00:00:00.000Z");
    expect(Object.keys(filled?.sounds ?? {}).sort()).toEqual(["chat-message", "invite"]);
    expect(fs.existsSync(path.join(cacheDir, "sounds", "chat-message.wav"))).toBe(true);
    expect(await readManifest(cacheDir)).toEqual(filled);
    // Already filled: nothing to do.
    expect(
      await extractMissingSounds({ disc: { kind: "folder", path: path.join(tmpRoot, "absent") }, cacheDir }),
    ).toEqual(filled);
  });

  it("extracts again a sound the catalog has replaced since (the chat sound was Brawl's shared Reflector)", async () => {
    const discRoot = makeDisc(path.join(tmpRoot, "disc6"));
    const cacheDir = path.join(tmpRoot, "cache6");
    const full = await runExtraction(await DiscFolderSource.open(discRoot), null, { cacheDir });
    expect(soundsOutdated(full)).toBe(false);

    const old = JSON.parse(JSON.stringify(full)) as typeof full;
    old.sounds!["chat-message"] = { ...old.sounds!["chat-message"], sound: "snd_se_fox_Special_L01", durationMs: 2027 };
    old.warnings.push("sound chat-message (sd:snd_se_fox_Special_L01): stale");
    fs.writeFileSync(path.join(cacheDir, "sounds", "chat-message.wav"), "stale");
    fs.writeFileSync(path.join(cacheDir, "manifest.json"), JSON.stringify(old));
    expect(soundsOutdated(old)).toBe(true);

    const filled = await extractMissingSounds({ disc: { kind: "folder", path: discRoot }, cacheDir });
    expect(filled?.sounds?.["chat-message"]).toMatchObject({ sound: soundName("chat-message"), durationMs: 20 });
    expect(filled?.sounds?.invite).toEqual(full.sounds?.invite);
    expect(filled?.warnings).toEqual(full.warnings);
    expect(decodeWav(fs.readFileSync(path.join(cacheDir, "sounds", "chat-message.wav"))).sampleRate).toBe(32000);
    expect(soundsOutdated(filled!)).toBe(false);
  });

  it("does not count a sound recorded missing as outdated, but does count a role added since", () => {
    const invite = { sound: soundName("invite") };
    expect(soundsOutdated({ sounds: { invite }, missing: ["sound:chat-message"] })).toBe(false);
    expect(soundsOutdated({ sounds: { invite }, missing: [] })).toBe(true);
    expect(soundsOutdated({ missing: [] })).toBe(true);
  });
});
