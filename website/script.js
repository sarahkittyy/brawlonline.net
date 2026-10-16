// Brawl Online landing page.

// ---- Config ----------------------------------------------------------------
// Download links per OS. Placeholders until the first builds exist. Keep the hrefs
// in index.html (the no-JavaScript fallback) in step with these.
const DOWNLOADS = {
  windows: "/downloads/BrawlOnline-Setup.exe",
  macos: "/downloads/BrawlOnline.dmg",
  linux: "/downloads/BrawlOnline.AppImage",
};
const OS_NAMES = { windows: "Windows", macos: "macOS", linux: "Linux" };
// The launcher's update feed (electron-updater). The button links the installer it names, so the
// version and size under the button are always those of the file it downloads.
const FEED_DIR = "/updates/launcher/";
const FEEDS = {
  windows: { feed: "latest.yml", file: /\.exe$/ },
  macos: { feed: "latest-mac.yml", file: /\.dmg$/, note: "Apple Silicon" },
  linux: { feed: "latest-linux.yml", file: /\.AppImage$/ },
};
// ----------------------------------------------------------------------------

/**
 * "windows" | "macos" | "linux" | "mobile" | "unknown".
 * Pure, so it can be tested with any inputs: detectOS({ ua, platform, touchPoints, uaData }).
 */
function detectOS(env) {
  const ua = env.ua || "";
  const platform = env.platform || "";
  const uaData = env.uaData || null;
  // ChromeOS: none of the three builds is a fit there.
  if (/CrOS/.test(ua) || (uaData && /chrome ?os/i.test(uaData.platform || ""))) return "unknown";
  if ((uaData && uaData.mobile) || /Android|iPhone|iPad|iPod|Mobile|Windows Phone/i.test(ua)) return "mobile";
  const p = ((uaData && uaData.platform) || platform || "").toLowerCase();
  if (/^win/.test(p) || /Windows NT/.test(ua)) return "windows";
  if (/mac/.test(p) || /Mac OS X|Macintosh/.test(ua)) {
    // iPadOS asks for the desktop site with a Mac user agent; only a touch screen tells it apart.
    return env.touchPoints > 1 ? "mobile" : "macos";
  }
  if (/linux/.test(p) || /Linux/.test(ua)) return "linux"; // Includes the Steam Deck.
  return "unknown";
}

function currentOS() {
  return detectOS({
    ua: navigator.userAgent,
    platform: navigator.platform,
    touchPoints: navigator.maxTouchPoints || 0,
    uaData: navigator.userAgentData || null,
  });
}

function setupDownloads(os) {
  const dl = document.getElementById("dl");
  const more = document.getElementById("more");
  const links = document.querySelectorAll("#other a[data-os]");
  links.forEach((a) => {
    a.href = DOWNLOADS[a.dataset.os];
    a.setAttribute("aria-label", "Download for " + OS_NAMES[a.dataset.os]);
  });

  const own = document.querySelector('#other a[data-os="' + os + '"]');
  if (own) {
    dl.href = DOWNLOADS[os];
    dl.querySelector("svg").replaceWith(own.querySelector("svg").cloneNode(true));
    dl.querySelector(".dl-label").textContent = "Download for " + OS_NAMES[os];
    showRelease(os, dl);
  } else {
    // Unknown OS or a phone: the neutral icon opens the list of all three.
    dl.href = "#other";
    dl.addEventListener("click", (e) => {
      e.preventDefault();
      more.open = true;
      more.querySelector("a").focus({ preventScroll: true });
    });
  }
}

function setupVideo() {
  const video = document.querySelector("video.bg");
  if (!video) return;
  const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");
  const saveData = navigator.connection && navigator.connection.saveData;
  const sync = () => {
    if (reduce.matches || saveData) {
      video.pause();
    } else {
      video.play().catch(() => {}); // A missing file or a blocked autoplay leaves the dark page.
    }
  };
  sync();
  reduce.addEventListener("change", sync);
}

/**
 * { version, files: [{ url, size }] } from an electron-updater feed (latest*.yml).
 * Pure, so it can be tested with any feed text.
 */
function parseFeed(text) {
  const unquote = (v) => v.trim().replace(/^'(.*)'$/, "$1").replace(/^"(.*)"$/, "$1");
  const out = { version: "", files: [] };
  for (const line of text.split(/\r?\n/)) {
    let m;
    if ((m = /^version:(.*)$/.exec(line))) out.version = unquote(m[1]);
    else if ((m = /^  - url:(.*)$/.exec(line))) out.files.push({ url: unquote(m[1]), size: 0 });
    else if ((m = /^    size:(.*)$/.exec(line)) && out.files.length) {
      out.files[out.files.length - 1].size = Number(unquote(m[1])) || 0;
    }
  }
  return out;
}

function formatSize(bytes) {
  return Math.round(bytes / (1024 * 1024)) + " MB";
}

// Points the button at the feed's newest installer and writes "v0.1.46 · 95 MB" under it. If the
// feed can't be read, the button keeps the stable /downloads/ link and the line stays empty.
async function showRelease(os, dl) {
  const meta = document.getElementById("dl-meta");
  const conf = FEEDS[os];
  try {
    const res = await fetch(FEED_DIR + conf.feed, { cache: "no-cache" });
    if (!res.ok) return;
    const feed = parseFeed(await res.text());
    const file = feed.files.find((f) => conf.file.test(f.url));
    if (!feed.version || !file) return;
    const url = FEED_DIR + encodeURIComponent(file.url);
    let size = file.size;
    if (!size) {
      // The Windows feed gives no size; ask the server.
      const head = await fetch(url, { method: "HEAD" });
      size = head.ok ? Number(head.headers.get("Content-Length")) || 0 : 0;
    }
    dl.href = url;
    meta.textContent = ["v" + feed.version, size && formatSize(size), conf.note].filter(Boolean).join(" \u00b7 ");
  } catch (e) {
    // Offline or blocked: the stable link still works.
  }
}

// The FAQ and Credits dialogs, opened by the links at the top left (and by visiting /#faq or
// /#credits). The Credits link at the bottom of the FAQ swaps one for the other.
function setupDialogs() {
  if (typeof HTMLDialogElement !== "function") return; // Falls back to the :target view.
  // Opened as modals, the :target fallback must not show them a second time.
  document.documentElement.classList.add("js-dialog");
  for (const id of ["faq", "credits"]) setupDialog(id);
}

function setupDialog(id) {
  const dialog = document.getElementById(id);
  if (!dialog) return;
  const show = () => {
    for (const other of document.querySelectorAll("dialog[open]")) if (other !== dialog) other.close();
    if (!dialog.open) dialog.showModal();
  };
  for (const open of document.querySelectorAll(`a[href="#${id}"]`)) {
    open.addEventListener("click", (e) => {
      e.preventDefault();
      show();
    });
  }
  dialog.querySelector(".faq-close").addEventListener("click", (e) => {
    e.preventDefault();
    dialog.close();
  });
  // A click on the backdrop (the dialog element itself, outside its padding box) closes it.
  dialog.addEventListener("click", (e) => {
    if (e.target !== dialog) return;
    const r = dialog.getBoundingClientRect();
    const inside = e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom;
    if (!inside) dialog.close();
  });
  dialog.addEventListener("close", () => {
    if (location.hash === `#${id}`) history.replaceState(null, "", location.pathname + location.search);
  });
  const fromHash = () => {
    if (location.hash === `#${id}`) show();
  };
  window.addEventListener("hashchange", fromHash);
  fromHash();
}

// Rank badges (docs/design/ranked-tiers.md): Layerlab's 2D League Icon Pack with each division's numeral baked
// on. Hovering names the rank.
const RANK_TIERS = {
  bronze: "Bronze", silver: "Silver", gold: "Gold", emerald: "Emerald",
  diamond: "Diamond", master: "Master", legendary: "Legendary",
};
// The emblems' common silhouette (256 x 256), drawn dotted while a player is placing.
const RANK_OUTLINE =
  "M123 9 L112 16 L94 34 L49 35 L32 39 L27 46 L26 64 L17 69 L16 81 L22 116 L27 121 L30 174 L36 191 L52 207 " +
  "L75 223 L116 247 L131 249 L142 245 L194 213 L209 201 L220 188 L225 165 L227 124 L236 116 L240 97 L242 72 " +
  "L238 66 L228 65 L227 46 L222 39 L203 35 L159 34 L136 12 L130 9 Z";

// The badge for a leaderboard entry, or null without a rank (an older server).
function rankBadge(entry) {
  const badge = document.createElement("span");
  badge.className = "rb";
  badge.tabIndex = 0;
  const left = Number(entry.setsToPlace) || 0;
  if (left > 0) {
    badge.dataset.tier = "placing";
    badge.dataset.name = left === 1 ? "Placing: 1 more ranked set" : "Placing: " + left + " more ranked sets";
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 256 256");
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", RANK_OUTLINE);
    svg.appendChild(path);
    badge.appendChild(svg);
    const tag = document.createElement("span");
    tag.className = "rb-tag";
    tag.textContent = left + " more";
    badge.appendChild(tag);
    return badge;
  }
  const rank = entry.rank;
  const name = rank && RANK_TIERS[rank.tier];
  if (!name) return null;
  const division = rank.tier === "legendary" ? 0 : Number(rank.division);
  if (rank.tier !== "legendary" && ![1, 2, 3].includes(division)) return null;
  const label = division ? name + " " + division : name;
  badge.dataset.tier = rank.tier;
  badge.dataset.name = label;
  const img = document.createElement("img");
  img.src = "assets/ranks/" + rank.tier + (division ? "-" + division : "") + ".png";
  img.alt = label;
  img.width = 128;
  img.height = 128;
  badge.appendChild(img);
  return badge;
}

// Ranked top 10 from the accounts API (same origin): rank badge and rating, one decimal, as the launcher
// shows it. Everything from the server goes in as text. If it can't be read, the section and its hint stay
// hidden.
async function setupTop10() {
  const section = document.getElementById("top10");
  const rows = document.getElementById("top-rows");
  if (!section || !rows) return;
  try {
    const res = await fetch("/v1/ranked/leaderboard?limit=10", { cache: "no-cache" });
    if (!res.ok) return;
    const page = await res.json();
    const entries = Array.isArray(page.entries) ? page.entries.slice(0, 10) : [];
    if (!entries.length) return;
    for (const e of entries) {
      const tr = document.createElement("tr");
      const cell = (cls, text) => {
        const td = document.createElement("td");
        td.className = cls;
        td.textContent = text;
        tr.appendChild(td);
      };
      cell("pos", String(e.position));
      cell("rank", "");
      const badge = rankBadge(e);
      if (badge) tr.lastChild.appendChild(badge);
      cell("name", String(e.displayName || ""));
      cell("code", String(e.connectCode || ""));
      cell("rating", Number(e.rating).toFixed(1));
      rows.appendChild(tr);
    }
    const total = Number(page.total) || 0;
    document.getElementById("top-total").textContent = total === 1 ? "1 ranked player" : total + " ranked players";
    section.hidden = false;
    document.getElementById("top-hint").hidden = false;
  } catch (e) {
    // Offline or blocked: the page works without it.
  }
}

window.BrawlOnline = { detectOS, parseFeed, DOWNLOADS };
setupDownloads(currentOS());
setupVideo();
setupDialogs();
setupTop10();
