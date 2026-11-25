/**
 * The changelog's HTML as the popup shows it. accounts renders it with the blog's renderer, which
 * already writes raw HTML in the Markdown as text and links only http(s)/mailto; this keeps only the
 * elements and attributes that renderer writes anyway, so nothing else (a script, an event handler, an
 * outside picture) can reach the page whatever the server sends. Addresses are made absolute on the
 * accounts server (the pictures are `/blog/media/...` there), and every link opens in the browser
 * (target=_blank, which the main window hands to the system).
 */

/** Elements kept as they are (their children are cleaned too). */
const KEEP = new Set([
  "P",
  "H2",
  "H3",
  "H4",
  "H5",
  "H6",
  "UL",
  "OL",
  "LI",
  "A",
  "IMG",
  "VIDEO",
  "FIGURE",
  "FIGCAPTION",
  "STRONG",
  "EM",
  "DEL",
  "CODE",
  "PRE",
  "BLOCKQUOTE",
  "HR",
  "BR",
  "TABLE",
  "THEAD",
  "TBODY",
  "TR",
  "TH",
  "TD",
  "INPUT",
  "SUP",
  "DIV",
  "SPAN",
]);

/** Elements dropped with everything in them. Any other element is replaced by its children. */
const DROP = new Set(["SCRIPT", "STYLE", "IFRAME", "OBJECT", "EMBED", "TEMPLATE", "NOSCRIPT", "SVG", "MATH", "FORM"]);

const LINK_PROTOCOLS = new Set(["http:", "https:", "mailto:"]);

function resolve(url: string, base: URL): URL | null {
  try {
    return new URL(url, base);
  } catch {
    return null;
  }
}

function clean(node: Node, doc: Document, base: URL): Node[] {
  if (node.nodeType === Node.TEXT_NODE) {
    return [doc.createTextNode(node.textContent ?? "")];
  }
  if (node.nodeType !== Node.ELEMENT_NODE) {
    return [];
  }
  const el = node as Element;
  const tag = el.tagName.toUpperCase();
  if (DROP.has(tag)) {
    return [];
  }
  const children = () => Array.from(el.childNodes).flatMap((c) => clean(c, doc, base));
  if (!KEEP.has(tag)) {
    return children();
  }
  const out = doc.createElement(tag.toLowerCase());
  switch (tag) {
    case "A": {
      const href = el.getAttribute("href") ?? "";
      const url = href.startsWith("#") ? null : resolve(href, base);
      if (!url || !LINK_PROTOCOLS.has(url.protocol)) {
        // A footnote's "#1" or anything not openable: only its text.
        return children();
      }
      out.setAttribute("href", url.href);
      out.setAttribute("target", "_blank");
      out.setAttribute("rel", "noopener noreferrer");
      out.setAttribute("title", url.href);
      break;
    }
    case "IMG":
    case "VIDEO": {
      // Only from the accounts server (the renderer writes an outside picture as a link).
      const url = resolve(el.getAttribute("src") ?? "", base);
      if (!url || url.origin !== base.origin) {
        return [];
      }
      out.setAttribute("src", url.href);
      if (tag === "IMG") {
        out.setAttribute("alt", el.getAttribute("alt") ?? "");
      } else {
        out.setAttribute("controls", "");
        out.setAttribute("preload", "metadata");
        out.setAttribute("playsinline", "");
        const label = el.getAttribute("aria-label");
        if (label) {
          out.setAttribute("aria-label", label);
        }
      }
      return [out];
    }
    case "INPUT": {
      // A task list's box.
      if ((el.getAttribute("type") ?? "").toLowerCase() !== "checkbox") {
        return [];
      }
      out.setAttribute("type", "checkbox");
      out.setAttribute("disabled", "");
      if (el.hasAttribute("checked")) {
        out.setAttribute("checked", "");
      }
      return [out];
    }
    case "TH":
    case "TD": {
      const align = /^text-align:\s*(left|center|right);?$/i.exec(el.getAttribute("style")?.trim() ?? "");
      if (align) {
        out.setAttribute("style", `text-align: ${align[1].toLowerCase()}`);
      }
      break;
    }
  }
  out.append(...children());
  return [out];
}

/** `html` (the server's) with only what the popup allows, addresses absolute on `base` (the accounts server). */
export function changelogHtml(html: string, base: string): string {
  const baseUrl = resolve(base.replace(/\/*$/, "/"), new URL("https://invalid/"));
  if (!baseUrl) {
    return "";
  }
  // An inert document: nothing in it loads or runs while it is read.
  const parsed = new DOMParser().parseFromString(`<!doctype html><body>${html}</body>`, "text/html");
  const box = document.createElement("div");
  box.append(...Array.from(parsed.body.childNodes).flatMap((n) => clean(n, document, baseUrl)));
  return box.innerHTML;
}

/**
 * Whether the popup shows this changelog: published after the last one shown (an edit of that one, or
 * an older one coming back, doesn't count), and before this launcher was built (one written for a
 * release still to come waits for that release).
 */
export function isNewChangelog(publishedAt: string, seenAt: string | undefined, builtAt: string): boolean {
  const published = Date.parse(publishedAt);
  if (Number.isNaN(published)) {
    return false;
  }
  const built = Date.parse(builtAt);
  if (!Number.isNaN(built) && published > built) {
    return false;
  }
  const seen = seenAt ? Date.parse(seenAt) : NaN;
  return Number.isNaN(seen) || published > seen;
}
