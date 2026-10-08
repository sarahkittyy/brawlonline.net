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
    dl.setAttribute("aria-label", "Download for " + OS_NAMES[os]);
    dl.replaceChildren(own.querySelector("svg").cloneNode(true));
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

window.BrawlOnline = { detectOS, DOWNLOADS };
setupDownloads(currentOS());
setupVideo();
