import type { SpecialRow } from "@common/rulesets";

export const SpecialMessages = {
  intro: () =>
    "P+'s Special Versus modes, as its Special Versus box has them. Players see them on the SPECIAL page of the room's rules.",
  off: () => "Off",
  row: (row: SpecialRow) =>
    ({
      stamina: "Stamina",
      size: "Size",
      attack: "Attack",
      body: "Body",
      status: "Status",
      gravity: "Gravity",
      speed: "Speed",
      other: "Other",
    }[row]),
  value: (value: string) =>
    ({
      "300": "300%",
      stamina: "Stamina",
      mega: "Mega",
      mini: "Mini",
      element: "Random Element",
      regen: "Regen",
      metal: "Metal",
      clear: "Clear",
      turbo: "Turbo Mode",
      bunny: "Bunny",
      light: "Light",
      heavy: "Heavy",
      wild: "Wild",
      fast: "Fast",
      meter: "Meter",
      "all-star": "All-Star",
    }[value] ?? value),
  /** P+'s own descriptions (its Special Versus box), with what online adds. */
  help: (value: string) =>
    ({
      "300": "Start with 300% damage! One-hit KOs!",
      stamina: "When your stamina runs out, you're KO'd! Each player picks their HP on their character panel.",
      mega: "Fight a huge battle while giant sized!",
      mini: "Fight a tiny battle while miniaturized!",
      element: "Fight with random hit effects!",
      regen:
        "Your damage resets if you can avoid being hit for a second. Stock control is then P+'s frame control, in any style.",
      metal: "Play in metal form, making you flinch less.",
      clear: "Play with invisible characters!",
      turbo: "Cancel moves on-hit for maximum combo potential!",
      bunny: "Wear a Bunny Hood, boosting performance.",
      light: "Jump higher and fall more slowly.",
      heavy: "Jump lower and fall faster.",
      wild: "No! This isn't how you're supposed to play the game!",
      fast: "Play at a bewildering speed!",
      meter: "Charge a Final Smash while fighting!",
      "all-star":
        "Fight with a different character per stock! Each player places their coin on every character of their roster; X with the coin in hand takes the last one off.",
    }[value] ?? ""),
  staminaType: () => "Stamina type",
  /** P+'s description of each stamina type, as the game's footer shows it. */
  staminaTypeHelp: (type: number) =>
    [
      "Set knockback with blastzones.",
      "Set knockback without blastzones.",
      "Normal knockback without blastzones.",
      "Normal knockback with blastzones.",
      "Normal knockback, no blastzones, and stabilized camera.",
    ][type - 1] ?? "",
  summary: (n: number) => (n === 0 ? "Off" : `${n} on`),
};
