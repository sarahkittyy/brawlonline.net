// The same names as the Credits in the repository's README.md and the website's FAQ; keep the three
// in step. Names are not translated, so they live here and not in a .messages.ts file.

export type Credit = { name: string; url: string; note?: string };

export const BRAWLBACK_CONTRIBUTORS: Credit[] = [
  { name: "WhiteTPoison", url: "https://github.com/WhiteTPoison" },
  { name: "PiNE", url: "https://github.com/FaultyPine" },
  { name: "Kapedani", url: "https://github.com/ilazoja" },
  { name: "barzamin", url: "https://github.com/barzamin" },
  { name: "Believer1", url: "https://github.com/Believer1" },
  { name: "blasphemetheus", url: "https://github.com/blasphemetheus" },
  { name: "eliasreid", url: "https://github.com/eliasreid" },
  { name: "pdeblois", url: "https://github.com/pdeblois" },
  { name: "rapito", url: "https://github.com/rapito" },
  { name: "WHellhaus", url: "https://github.com/WHellhaus" },
];

export const BUILT_ON: Credit[] = [
  {
    name: "Nyx",
    url: "https://github.com/NyxTheShield",
    note: "Brawlback rollback fork",
  },
  {
    name: "Orca",
    url: "https://github.com/yougame-co/orca-netplay",
    note: "YouGame",
  },
  {
    name: "Syriinge",
    url: "https://github.com/Sammi-Husky/Syriinge",
    note: "Sammi Husky",
  },
  {
    name: "GekkoNet",
    url: "https://github.com/HeatXD/GekkoNet",
    note: "HeatXD",
  },
  {
    name: "Slippi Launcher",
    url: "https://github.com/project-slippi/slippi-launcher",
    note: "Project Slippi",
  },
  { name: "Dolphin", url: "https://github.com/dolphin-emu/dolphin" },
  {
    name: "Project+",
    url: "https://projectplusgame.com",
    note: "the Project+ Development Team",
  },
];

export const BRAWLBACK_URL = "https://github.com/Brawlback-Team";
