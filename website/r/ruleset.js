// brawlonline.net/r/<code>: shows a published ruleset (GET /v1/rulesets/<code>) and opens the
// launcher with brawlonline://room/create?ruleset=<code> (&private=1 for a private room;
// docs/rulesets-protocol.md §4).
// Everything from the server goes into the page as text (textContent), never as HTML: names are
// typed by whoever published the ruleset.
"use strict";

(() => {
  const CODE_RE = /^[BCDFGHJKLMNPQRSTVWXZ]{6}$/;

  // P+ v3.2's legal stages (srStageKind), as the launcher names them (launcher/src/common/rulesets.ts).
  const STAGES = {
    1: "Battlefield",
    2: "Final Destination",
    3: "Delfino's Secret",
    4: "Luigi's Mansion",
    5: "Metal Cavern",
    6: "Bowser's Castle",
    9: "Temple of Time",
    12: "Frigate Husk",
    13: "Yoshi's Island",
    28: "Wario Land",
    31: "Fountain of Dreams",
    33: "Smashville",
    35: "Green Hill Zone",
    45: "Dream Land",
    46: "Pokémon Stadium 2",
  };
  const stageName = (id) => STAGES[id] || `Stage ${id}`;

  // P+'s ITEM SWITCH, in its screen order = bit order (launcher/src/common/items.ts, docs/game-code.md §14):
  // bit i of the 8 bytes read as one big-endian number. The last four are P+ settings, not items.
  const ITEMS = [
    "Smash Ball", "Assist Trophy", "Poké Ball", "Container", "Blast Box", "Sandbag", "Food", "Maxim Tomato",
    "Heart Container", "Dragoon Parts", "Super Mushroom", "Poison Mushroom", "Warp Star", "Starman", "Metal Box",
    "Bunny Hood", "Turbo", "Timer", "Lightning", "Beam Sword", "Home-Run Bat", "Fan", "Lip's Stick", "Star Rod",
    "Hammer", "Golden Hammer", "Ray Gun", "Super Scope", "Fire Flower", "Cracker Launcher", "Bob-omb",
    "Motion-Sensor Bomb", "Gooey Bomb", "Smart Bomb", "Deku Nut", "Freezie", "Smoke Ball", "Pitfall", "Hothead",
    "Mr. Saturn", "Green Shell", "Banana Peel", "Bumper", "Spring", "Unira", "Soccer Ball", "Team Healer",
    "Franklin Badge", "Screw Attack",
  ];
  const ITEM_SETTINGS = ["Stage Items", "EX Items", "Mayhem Mode", "Passive-Aggression"];
  const itemOn = (sw, bit) => (((sw || [])[7 - (bit >> 3)] || 0) & (1 << (bit & 7))) !== 0;

  /** The code from /r/<code>; /r/?<code> and /r/#<code> also work (a plain static server). */
  function codeFromLocation(loc) {
    const parts = loc.pathname.split("/").filter(Boolean);
    const candidates = [parts[1], loc.search.replace(/^\?(code=)?/, ""), loc.hash.replace(/^#/, "")];
    for (const c of candidates) {
      const code = (c || "").trim().toUpperCase();
      if (CODE_RE.test(code)) {
        return code;
      }
    }
    return null;
  }

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  };
  const onOff = (b) => (b ? "On" : "Off");

  // The default, and the stages with their own (`hazardStages`, the other way): "Off (on: Fountain of Dreams)".
  function hazardsText(rules) {
    const own = rules.hazardStages || [];
    if (own.length === 0) return onOff(rules.hazards);
    return `${onOff(rules.hazards)} (${rules.hazards ? "off" : "on"}: ${own.map(stageName).join(", ")})`;
  }

  function rulesRows(rules) {
    const style = { stock: "Stock", time: "Time", coin: "Coin" }[rules.style] || rules.style;
    const time = rules.timeMinutes === 0 ? "No limit" : `${rules.timeMinutes} min`;
    const rows = [["Style", style]];
    if (rules.style === "stock") rows.push(["Stocks", String(rules.stocks)]);
    // (as the game plays it: stock matches, or any style with regen's frame control; not with stamina)
    const special = rules.special || {};
    const regen = special.attack === "regen";
    if (rules.stockControl && (rules.style === "stock" || regen) && special.stamina !== "stamina") {
      rows.push(regen ? ["Frame control", "On: each player picks theirs"] : ["Stock control", "On: each player picks their stocks"]);
    }
    rows.push(
      ["Time limit", time],
      ["Damage ratio", Number(rules.damageRatio).toFixed(1)],
      ["Team attack", onOff(rules.teamAttack)],
      ["Pause", rules.pause === "hold" ? "Hold START" : "On"],
      ["Stage hazards", hazardsText(rules)],
      ["Auto L-cancel", onOff(rules.autoLCancel)],
      ["Input assist", onOff(rules.inputAssist)],
    );
    return rows;
  }

  /** Items as P+ plays them: none when the frequency is None or no item is on. */
  function itemsGroup(rules) {
    const box = el("div", "group");
    box.appendChild(el("h2", null, "Items"));
    const on = ITEMS.filter((_, bit) => itemOn(rules.itemSwitch, bit));
    const freq =
      ["None", "Low", "Medium", "High", "Very High", "Intense", "Bomb Rain"][rules.itemFrequency] ||
      String(rules.itemFrequency);
    const dl = el("dl");
    const row = (k, v) => {
      dl.appendChild(el("dt", null, k));
      dl.appendChild(el("dd", null, v));
    };
    if (rules.itemFrequency === 6) {
      row("Items", "Bomb Rain (sudden death's Bob-ombs)");
    } else if (rules.itemFrequency === 0 || on.length === 0) {
      row("Items", "None");
    } else {
      row("Items", on.length === ITEMS.length ? "All items" : `${on.length} of ${ITEMS.length}`);
      row("Frequency", freq);
    }
    const settings = ITEM_SETTINGS.filter((_, i) => itemOn(rules.itemSwitch, ITEMS.length + i));
    row("P+ settings", settings.length ? settings.join(", ") : "None");
    box.appendChild(dl);
    if (rules.itemFrequency > 0 && on.length > 0 && on.length < ITEMS.length) {
      const ul = el("ul", "stages");
      for (const name of on) ul.appendChild(el("li", null, name));
      box.appendChild(ul);
    }
    return box;
  }

  // P+'s Special Versus box (docs/design/special-rules.md): the rows that are on.
  const SPECIAL_ROWS = [
    ["stamina", "Stamina", { 300: "300%", stamina: "Stamina (each player picks their HP)" }],
    ["size", "Size", { mega: "Mega", mini: "Mini" }],
    ["attack", "Attack", { element: "Random Element", regen: "Regen" }],
    ["body", "Body", { metal: "Metal", clear: "Clear" }],
    ["status", "Status", { turbo: "Turbo Mode", bunny: "Bunny" }],
    ["gravity", "Gravity", { light: "Light", heavy: "Heavy" }],
    ["speed", "Speed", { wild: "Wild", fast: "Fast" }],
    ["other", "Other", { meter: "Meter", "all-star": "All-Star (a character per stock)" }],
  ];

  function specialRows(rules) {
    const special = rules.special || {};
    const rows = [];
    for (const [key, label, values] of SPECIAL_ROWS) {
      if (special[key] && special[key] !== "off") rows.push([label, values[special[key]] || special[key]]);
    }
    if (special.stamina === "stamina") rows.push(["Stamina type", String(rules.staminaType || 1)]);
    return rows;
  }

  function formatRows(r) {
    const rows = [];
    if (r.bestOf > 0) {
      rows.push(["Set count", `Best of ${r.bestOf}`]);
      rows.push(["Game 1", `Strike ${r.strikes.join("-")} from the starters`]);
      rows.push(["Games 2+", `Winner bans ${r.bans}, loser picks`]);
      rows.push(["DSR", r.dsr ? "On (no picking a stage you won on)" : "Off"]);
    } else {
      const choice = {
        "loser-picks": "Game 1 random, then the loser picks",
        strike: `Strike ${r.strikes.join("-")} from the starters every game`,
        random: "Random every game",
        "anyone-picks": "Every game, anyone picks any stage: the first pick plays",
      }[r.stageChoice];
      rows.push(["Set count", "No set: one game at a time"]);
      rows.push(["Stages", choice || r.stageChoice]);
    }
    return rows;
  }

  function table(title, rows) {
    const box = el("div", "group");
    if (title) box.appendChild(el("h2", null, title));
    const dl = el("dl");
    for (const [k, v] of rows) {
      dl.appendChild(el("dt", null, k));
      dl.appendChild(el("dd", null, v));
    }
    box.appendChild(dl);
    return box;
  }

  function stageList(title, ids) {
    const box = el("div", "group");
    box.appendChild(el("h2", null, `${title} (${ids.length})`));
    const ul = el("ul", "stages");
    for (const id of ids) ul.appendChild(el("li", null, stageName(id)));
    if (ids.length === 0) ul.appendChild(el("li", "none", "None"));
    box.appendChild(ul);
    return box;
  }

  function render(view) {
    const r = view.ruleset;
    const card = document.getElementById("card");
    card.textContent = "";

    const head = el("div", "head");
    const titles = el("div");
    titles.appendChild(el("h1", "name", r.name));
    const meta = el("p", "meta");
    meta.appendChild(el("span", "version", `v${r.version}`));
    meta.appendChild(document.createTextNode(" · by "));
    meta.appendChild(el("span", "by", r.by));
    titles.appendChild(meta);
    head.appendChild(titles);
    const code = el("button", "code", view.code);
    code.type = "button";
    code.title = "Copy the code";
    code.addEventListener("click", () => {
      navigator.clipboard?.writeText(view.code).then(() => {
        code.classList.add("copied");
        setTimeout(() => code.classList.remove("copied"), 1200);
      });
    });
    head.appendChild(code);
    card.appendChild(head);

    if (view.newer) {
      const newer = el("p", "newer");
      newer.appendChild(document.createTextNode("A newer version exists: "));
      const a = el("a", null, `v${view.newer.version} (${view.newer.code})`);
      a.href = `/r/${view.newer.code}`;
      newer.appendChild(a);
      card.appendChild(newer);
    }

    const grid = el("div", "grid");
    grid.appendChild(table(null, formatRows(r)));
    grid.appendChild(table("Rules", rulesRows(r.rules)));
    const special = specialRows(r.rules);
    if (special.length > 0) grid.appendChild(table("Special", special));
    grid.appendChild(itemsGroup(r.rules));
    grid.appendChild(stageList(r.bestOf > 0 || r.stageChoice === "strike" ? "Starters" : "Stages", r.stages.starters));
    if (r.stages.counterpicks.length > 0 || r.bestOf > 0) {
      grid.appendChild(stageList("Counterpicks", r.stages.counterpicks));
    }
    card.appendChild(grid);

    document.title = `${r.name} v${r.version} by ${r.by} · Brawl Online`;
    for (const [id, query] of [["create-public", ""], ["create-private", "&private=1"]]) {
      const create = document.getElementById(id);
      create.href = `brawlonline://room/create?ruleset=${view.code}${query}`;
      // If nothing takes the link (no launcher), the page keeps focus: point at the download.
      create.addEventListener("click", () => {
        let left = false;
        const away = () => (left = true);
        window.addEventListener("blur", away, { once: true });
        setTimeout(() => {
          window.removeEventListener("blur", away);
          if (!left && !document.hidden) document.getElementById("no-launcher").classList.add("show");
        }, 1500);
      });
    }
    document.getElementById("actions").hidden = false;
  }

  function fail(text) {
    const status = document.getElementById("status");
    status.textContent = text;
    status.classList.add("error");
  }

  async function main() {
    const code = codeFromLocation(window.location);
    if (!code) {
      fail("That isn't a ruleset code. Codes are 6 letters, like brawlonline.net/r/KFQBTR.");
      return;
    }
    let res;
    try {
      res = await fetch(`/v1/rulesets/${code}`, { headers: { accept: "application/json" } });
    } catch {
      fail("Can't reach the server right now. Try again in a moment.");
      return;
    }
    if (res.status === 404) {
      fail(`No ruleset has the code ${code}.`);
      return;
    }
    if (!res.ok) {
      fail(res.status === 429 ? "Too many requests. Try again in a minute." : `The server answered ${res.status}.`);
      return;
    }
    render(await res.json());
  }

  window.BrawlOnlineRuleset = { codeFromLocation };
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", main);
  } else {
    main();
  }
})();
