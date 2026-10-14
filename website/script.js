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

// FAQ dialog, opened by the "FAQ" link at the top left (and by visiting /#faq).
function setupFaq() {
  const dialog = document.getElementById("faq");
  const open = document.getElementById("faq-open");
  const close = document.getElementById("faq-close");
  if (!dialog || typeof dialog.showModal !== "function") return; // Falls back to the :target view.
  // Opened as a modal, the :target fallback must not show it a second time.
  document.documentElement.classList.add("js-dialog");
  const show = () => {
    if (!dialog.open) dialog.showModal();
  };
  open.addEventListener("click", (e) => {
    e.preventDefault();
    show();
  });
  close.addEventListener("click", (e) => {
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
    if (location.hash === "#faq") history.replaceState(null, "", location.pathname + location.search);
  });
  const fromHash = () => {
    if (location.hash === "#faq") show();
  };
  window.addEventListener("hashchange", fromHash);
  fromHash();
}

// Ranked top 10 from the accounts API (same origin). Rating only, one decimal, as the launcher shows it.
// Everything from the server goes in as text. If it can't be read, the section and its hint stay hidden.
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
setupFaq();
setupTop10();
