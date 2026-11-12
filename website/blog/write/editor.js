/* The blog editor (/blog/write/, docs/design/blog.md §3). Admins only. No hash: the list of posts;
   #<id>: that post, Markdown on the left, accounts' own rendering of it on the right.

   - Drafts save themselves 1.5 s after the last change. A published post's changes wait for
     Update (Ctrl+S saves either), so half-typed sentences never go live.
   - Every change is also kept in localStorage until the server has it; a reload or a closed tab
     offers it back.
   - Images and videos: paste, drop, or Upload. Each goes up at once (PNG/JPEG/BMP become WebP when
     that is smaller) and its Markdown line takes the place of an "Uploading" note.
   - A save over a change made elsewhere (another tab) is refused by the server, not overwritten. */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const api = BrawlAuth.api;
  const AUTOSAVE_MS = 1500;
  const PREVIEW_MS = 300;
  const MAX_UPLOAD = 95 * 1024 * 1024; // accounts' BLOG_MEDIA_MAX_BYTES default (Cloudflare caps at 100 MB)
  const WEBP_MAX_WIDTH = 2400;
  const FIELDS = ["title", "slug", "summary", "author", "body"];

  let post = null; // the server's copy of the open post
  let saving = null; // the save in flight (a Promise)
  let saveAgain = false; // something changed while it was in flight
  let autosaveTimer = 0;
  let previewTimer = 0;
  let previewSeq = 0;
  let backupTimer = 0;
  let slugFollowsTitle = false;
  let uploadSeq = 0;
  let problem = ""; // the last save's error, shown until the next good save

  // ---------------------------------------------------------------- helpers

  function toast(text, bad = false) {
    const t = $("toast");
    t.textContent = text;
    t.classList.toggle("bad", bad);
    t.hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => (t.hidden = true), bad ? 8000 : 3500);
  }

  function dateText(iso) {
    if (!iso) return "";
    return new Date(iso).toLocaleDateString("en-US", { month: "short", day: "numeric", year: "numeric" });
  }

  function timeText(iso) {
    return new Date(iso).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  }

  /** As accounts' blog::slugify: "Progress Report #1!" -> "progress-report-1". */
  function slugify(title) {
    let out = "";
    for (const c of title.toLowerCase()) {
      if (/[a-z0-9]/.test(c)) out += c;
      else if (out && !out.endsWith("-")) out += "-";
      if (out.length >= 80) break;
    }
    out = out.replace(/-+$/, "");
    return !out || out === "write" || out === "media" ? "post" : out;
  }

  function fields() {
    const f = {};
    FIELDS.forEach((k) => (f[k] = $(k).value));
    return f;
  }

  function differs(f, p) {
    return f.title.trim() !== p.title || f.slug.trim() !== p.slug || f.summary.trim() !== p.summary ||
      f.author.trim() !== p.author || f.body !== p.body;
  }

  function dirty() {
    return !!post && differs(fields(), post);
  }

  function backupKey(id) {
    return `brawlonline.blog.unsaved.${id}`;
  }

  // ---------------------------------------------------------------- start

  async function start() {
    const session = await BrawlAuth.requireLogin();
    BrawlAuth.mountAccount($("account"));
    if (!BrawlAuth.hasRole(session, "admin")) {
      $("gate-text").textContent =
        `${session.user.displayName} can't write the blog. Log out and in with an admin account (pp-admin user role <email> admin).`;
      return;
    }
    // Logged out in another tab: back to the login page (with this page to come back to).
    BrawlAuth.onChange((s) => {
      if (!s) location.assign(BrawlAuth.loginUrl());
    });
    window.addEventListener("hashchange", route);
    route();
  }

  function show(which) {
    $("gate").hidden = which !== "gate";
    $("list").hidden = which !== "list";
    $("edit").hidden = which !== "edit";
  }

  async function route() {
    const hash = decodeURIComponent(location.hash.slice(1));
    let id = Number(hash);
    if (hash && !(Number.isInteger(id) && id > 0)) {
      // #<address> (e.g. /blog/write/#progress-report-1): that post, by its link's last part.
      try {
        const found = (await api("/v1/blog/posts")).find((p) => p.slug === hash.toLowerCase());
        if (!found) throw new BrawlAuth.ApiError(404, "not_found", `No post has the address "${hash}".`);
        id = found.id;
        history.replaceState(null, "", `#${id}`);
      } catch (e) {
        toast(e.message, true);
        history.replaceState(null, "", location.pathname);
        id = 0;
      }
    }
    if (post && dirty() && post.id !== id) {
      // Leaving a post with changes: save them first (the backup has them either way).
      await save().catch(() => {});
    }
    if (Number.isInteger(id) && id > 0) await openPost(id);
    else await openList();
  }

  // ---------------------------------------------------------------- the list

  async function openList() {
    post = null;
    document.title = "Blog editor · Brawl Online";
    show("list");
    const rows = $("post-rows");
    $("list-error").hidden = true;
    try {
      const posts = await api("/v1/blog/posts");
      rows.replaceChildren(
        ...posts.map((p) => {
          const li = document.createElement("li");
          const a = document.createElement("a");
          a.href = `#${p.id}`;
          const status = document.createElement("span");
          status.className = `status ${p.status}`;
          status.textContent = p.status === "published" ? "Published" : "Draft";
          const title = document.createElement("span");
          title.className = `row-title${p.title ? "" : " untitled"}`;
          title.textContent = p.title || "Untitled";
          const date = document.createElement("span");
          date.className = "row-date";
          date.textContent = p.status === "published" ? dateText(p.publishedAt) : `edited ${dateText(p.updatedAt)}`;
          if (localStorage.getItem(backupKey(p.id))) {
            // Changes the server never got (a closed tab, or an edit to a published post that
            // was not Updated): opening the post offers them back.
            const unsaved = document.createElement("span");
            unsaved.className = "row-unsaved";
            unsaved.textContent = "Unsaved changes";
            unsaved.title = "Changes kept in this browser. Open the post to restore them.";
            title.append(unsaved);
          }
          a.append(status, title, date);
          li.append(a);
          return li;
        }),
      );
      $("list-empty").hidden = posts.length > 0;
    } catch (e) {
      $("list-error").textContent = e.message;
      $("list-error").hidden = false;
    }
  }

  $("new-post").addEventListener("click", async () => {
    $("new-post").disabled = true;
    try {
      const p = await api("/v1/blog/posts", { method: "POST", json: {} });
      location.hash = String(p.id);
    } catch (e) {
      toast(e.message, true);
    } finally {
      $("new-post").disabled = false;
    }
  });

  // ---------------------------------------------------------------- one post

  async function openPost(id) {
    show("edit");
    hideBanner();
    let p;
    try {
      p = await api(`/v1/blog/posts/${id}`);
    } catch (e) {
      toast(e.status === 404 ? "That post doesn't exist (any more)." : e.message, true);
      location.hash = "";
      return;
    }
    post = p;
    problem = "";
    FIELDS.forEach((k) => ($(k).value = p[k]));
    slugFollowsTitle = !p.publishedAt && (p.slug === slugify(p.title) || /^untitled(-\d+)?$/.test(p.slug));
    lockSlug(!!p.publishedAt);
    $("slug").closest(".slug-field").classList.remove("bad");
    $("slug-prefix").textContent = `${location.host}/blog/`;
    renderStatus();
    renderPreviewNow();
    offerBackup();
    if (!p.title) $("title").focus();
    else $("body").focus();
  }

  function renderStatus() {
    if (!post) return;
    const published = post.status === "published";
    const isDirty = dirty();
    $("status").textContent = published ? "Published" : "Draft";
    $("status").className = `status ${post.status}`;
    $("view").textContent = published ? "View post" : "Preview link";
    $("view").href = post.viewUrl;
    $("view").title = published ? "" : "Anyone with this link can read the draft";
    $("publish").textContent = published ? "Update" : "Publish";
    $("publish").disabled = published && !isDirty && !saving;
    $("unpublish").hidden = !published;
    const state = $("save-state");
    state.classList.remove("dirty", "bad");
    if (problem) {
      state.textContent = problem;
      state.classList.add("bad");
    } else if (saving) {
      state.textContent = "Saving…";
    } else if (isDirty) {
      state.textContent = published ? "Unsaved: Update to publish" : "Unsaved changes";
      state.classList.add("dirty");
    } else {
      state.textContent = `Saved ${timeText(post.updatedAt)}`;
    }
    document.title = `${post.title || "Untitled"} · Blog editor`;
  }

  /** Saves the open post. Resolves once the server has the fields as they were when called. */
  async function save() {
    clearTimeout(autosaveTimer);
    if (!post) return;
    if (saving) {
      saveAgain = true;
      return saving;
    }
    if (!dirty()) return;
    const f = fields();
    const id = post.id;
    saving = (async () => {
      try {
        const p = await api(`/v1/blog/posts/${id}`, {
          method: "PUT",
          json: { ...f, expectedUpdatedAt: post.updatedAt },
        });
        if (!post || post.id !== id) return;
        post = p;
        problem = "";
        $("slug").closest(".slug-field").classList.remove("bad");
        $("view").href = p.viewUrl;
        if (!dirty()) localStorage.removeItem(backupKey(id));
      } catch (e) {
        problem = e.message;
        if (e.code === "bad_slug" || e.code === "slug_taken") $("slug").closest(".slug-field").classList.add("bad");
        if (e.code === "changed_elsewhere") {
          showBanner(e.message, "Load that version", () => openPost(id), "Keep mine", () => {
            // Take the server's current version as the base, then save ours over it.
            api(`/v1/blog/posts/${id}`).then((p) => {
              post = p;
              problem = "";
              hideBanner();
              save();
            });
          });
        }
        throw e;
      } finally {
        saving = null;
        renderStatus();
        if (saveAgain) {
          saveAgain = false;
          if (post && post.status !== "published") scheduleAutosave();
        }
      }
    })();
    renderStatus();
    return saving;
  }

  function scheduleAutosave() {
    clearTimeout(autosaveTimer);
    if (post && post.status !== "published") autosaveTimer = setTimeout(() => save().catch(() => {}), AUTOSAVE_MS);
  }

  function changed() {
    if (!post) return;
    renderStatus();
    schedulePreview();
    scheduleAutosave();
    lineTops = null;
    clearTimeout(backupTimer);
    backupTimer = setTimeout(writeBackup, 300);
  }

  /** Keeps the open post's unsaved changes in localStorage (offered back when it is opened again). */
  function writeBackup() {
    clearTimeout(backupTimer);
    if (!post || !dirty()) return;
    try {
      localStorage.setItem(backupKey(post.id), JSON.stringify({ ...fields(), base: post.updatedAt, at: new Date().toISOString() }));
    } catch {
      // Storage full or off: the server copy (drafts save themselves) is all there is.
    }
  }

  // Closing the tab (or the browser) right after typing: keep the last keystrokes too.
  window.addEventListener("pagehide", writeBackup);
  document.addEventListener("visibilitychange", () => document.visibilityState === "hidden" && writeBackup());

  FIELDS.forEach((k) => $(k).addEventListener("input", changed));

  $("title").addEventListener("input", () => {
    if (slugFollowsTitle) $("slug").value = slugify($("title").value);
  });
  $("slug").addEventListener("input", () => {
    slugFollowsTitle = false;
  });

  // A post that was ever published keeps its address: links to it are out there. Edits and
  // Update keep the same post, link and id; only "change" (after a warning) unlocks the address.
  function lockSlug(locked) {
    $("slug").readOnly = locked;
    $("slug").title = locked ? "Published posts keep their address, so shared links keep working" : "";
    $("slug-change").hidden = !locked;
  }

  $("slug-change").addEventListener("click", () => {
    if (!post) return;
    const ok = confirm(
      `Change this post's address?\n\nLinks people already shared, and search results, point at /blog/${post.slug}. ` +
        "After the change that link shows \"No such post\".",
    );
    if (!ok) return;
    lockSlug(false);
    $("slug").focus();
    $("slug").select();
  });

  // A reload or a closed tab left changes the server never got: offer them back.
  function offerBackup() {
    let b = null;
    try {
      b = JSON.parse(localStorage.getItem(backupKey(post.id)) || "null");
    } catch {
      b = null;
    }
    if (!b || !differs({ ...b, title: b.title || "", summary: b.summary || "", author: b.author || "" }, post)) {
      localStorage.removeItem(backupKey(post.id));
      return;
    }
    const newer = b.base !== post.updatedAt ? " The post was saved again since then." : "";
    showBanner(
      `Unsaved changes from ${new Date(b.at).toLocaleString()} are still here.${newer}`,
      "Restore them",
      () => {
        FIELDS.forEach((k) => {
          if (typeof b[k] === "string") $(k).value = b[k];
        });
        hideBanner();
        changed();
      },
      "Throw them away",
      () => {
        localStorage.removeItem(backupKey(post.id));
        hideBanner();
      },
    );
  }

  function showBanner(text, yes, onYes, no, onNo) {
    $("banner-text").textContent = text;
    $("banner-yes").textContent = yes;
    $("banner-no").textContent = no;
    $("banner-yes").onclick = onYes;
    $("banner-no").onclick = onNo;
    $("banner").hidden = false;
  }

  function hideBanner() {
    $("banner").hidden = true;
  }

  $("back").addEventListener("click", (e) => {
    e.preventDefault();
    location.hash = "";
  });

  $("publish").addEventListener("click", async () => {
    if (!post) return;
    $("publish").disabled = true;
    try {
      await save();
      if (post.status !== "published") {
        if (!confirm(`Publish "${post.title || "Untitled"}" on the blog now?`)) return;
        post = await api(`/v1/blog/posts/${post.id}/publish`, { method: "POST" });
        slugFollowsTitle = false;
        lockSlug(true);
        toast("Published. It's on brawlonline.net/blog now.");
      } else {
        toast("Updated. The post shows the changes now.");
      }
    } catch (e) {
      toast(e.message, true);
    } finally {
      renderStatus();
    }
  });

  $("unpublish").addEventListener("click", async () => {
    if (!post || !confirm("Take this post off the blog? It goes back to being a draft.")) return;
    try {
      await save();
      post = await api(`/v1/blog/posts/${post.id}/unpublish`, { method: "POST" });
      toast("Unpublished: it's a draft again.");
    } catch (e) {
      toast(e.message, true);
    }
    renderStatus();
  });

  $("delete").addEventListener("click", async () => {
    if (!post || !confirm(`Delete "${post.title || "Untitled"}" for good?`)) return;
    try {
      await api(`/v1/blog/posts/${post.id}`, { method: "DELETE" });
      localStorage.removeItem(backupKey(post.id));
      post = null;
      toast("Deleted.");
      location.hash = "";
    } catch (e) {
      toast(e.message, true);
    }
  });

  document.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") {
      e.preventDefault();
      if (post) save().then(() => post.status === "published" && toast("Updated.")).catch((err) => toast(err.message, true));
    }
  });

  window.addEventListener("beforeunload", (e) => {
    if (dirty() || $("uploads").children.length) {
      e.preventDefault();
      e.returnValue = "";
    }
  });

  // ---------------------------------------------------------------- the preview

  function schedulePreview() {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(renderPreviewNow, PREVIEW_MS);
  }

  async function renderPreviewNow() {
    clearTimeout(previewTimer);
    const f = fields();
    const title = f.title.trim();
    $("p-title").textContent = title || "Untitled";
    $("p-title").classList.toggle("empty", !title);
    const seq = ++previewSeq;
    try {
      const r = await api("/v1/blog/render", { method: "POST", json: { markdown: f.body } });
      if (seq !== previewSeq) return;
      const parts = [post && post.publishedAt ? dateText(post.publishedAt) : "Not published yet"];
      if (f.author.trim()) parts.push(`by ${f.author.trim()}`);
      parts.push(`${r.readingMinutes} min read`);
      $("p-byline").textContent = parts.join(" · ");
      // accounts' renderer: raw HTML in the Markdown comes back as text.
      patchPreview(r.html);
      blockLines = Array.isArray(r.blocks) ? r.blocks : [];
      lineTops = null;
      syncPreview();
    } catch (e) {
      if (seq === previewSeq) $("p-byline").textContent = `Preview failed: ${e.message}`;
    }
  }

  /** The HTML each preview element was made from: unchanged ones are kept, not made again. */
  const madeFrom = new WeakMap();

  /**
   * Puts the rendered HTML in the preview, keeping every element whose HTML did not change, so a
   * playing video keeps playing and pictures don't load again (and jump) at each keystroke.
   */
  function patchPreview(html) {
    const box = $("p-body");
    const tpl = document.createElement("template");
    tpl.innerHTML = html;
    const old = new Map();
    for (const el of box.children) {
      const k = madeFrom.get(el);
      if (k === undefined) continue;
      if (!old.has(k)) old.set(k, []);
      old.get(k).push(el);
    }
    const keep = new Set();
    const next = [...tpl.content.children].map((el) => {
      const k = el.outerHTML;
      const same = old.get(k)?.shift();
      if (same) {
        keep.add(same);
        return same;
      }
      madeFrom.set(el, k);
      // The preview is the author's own pictures: load them all, so the heights are right for the
      // scrolling below.
      el.querySelectorAll("img[loading=lazy]").forEach((img) => (img.loading = "eager"));
      if (el.matches("img[loading=lazy]")) el.loading = "eager";
      return el;
    });
    [...box.childNodes].forEach((n) => keep.has(n) || n.remove());
    // Kept elements are only moved when they must be (a moved video would stop).
    let at = box.firstChild;
    for (const el of next) {
      if (el === at) at = at.nextSibling;
      else box.insertBefore(el, at);
    }
  }

  // ---------------------------------------------------------------- the preview follows the Markdown

  // The source line where each element of the preview starts (accounts' render), and the
  // Markdown's height at each of those lines (measured in a copy of the textarea, wrapped alike).
  let blockLines = [];
  let lineTops = null;
  let mirror = null;
  // What decides where the textarea wraps its lines.
  const MIRRORED = ["fontFamily", "fontSize", "fontWeight", "fontStyle", "fontStretch", "lineHeight",
    "letterSpacing", "wordSpacing", "tabSize", "textIndent", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft"];

  /** Pixel top of each line in `lines` (ascending), in the textarea's content. */
  function measureLines(lines) {
    const src = $("body");
    const cs = getComputedStyle(src);
    if (!mirror) {
      mirror = document.createElement("div");
      mirror.setAttribute("aria-hidden", "true");
      document.body.append(mirror);
    }
    const m = mirror.style;
    m.position = "absolute";
    m.left = "-10000px";
    m.top = "0";
    m.visibility = "hidden";
    m.boxSizing = "border-box";
    m.width = `${src.clientWidth}px`; // without the scrollbar, like the text inside
    m.whiteSpace = "pre-wrap";
    m.overflowWrap = "break-word";
    m.wordBreak = cs.wordBreak;
    for (const p of MIRRORED) m[p] = cs[p];
    m.border = "0";
    const text = src.value.split("\n");
    const marks = [];
    const parts = [];
    let from = 0;
    for (const line of lines) {
      if (line < from || line >= text.length) continue;
      parts.push(document.createTextNode(text.slice(from, line).map((t) => t + "\n").join("")));
      const mark = document.createElement("span");
      marks.push(mark);
      parts.push(mark);
      from = line;
    }
    parts.push(document.createTextNode(text.slice(from).join("\n")));
    mirror.replaceChildren(...parts);
    const tops = marks.map((mark) => mark.offsetTop);
    mirror.replaceChildren();
    return marks.length === lines.length ? tops : null;
  }

  /**
   * Scrolls the preview so it shows what the Markdown shows: the block at the top of the Markdown
   * is at the top of the preview, however tall its pictures and videos are; between two blocks
   * it moves in step. Both reach their ends together.
   */
  function syncPreview() {
    const src = $("body");
    const pane = $("preview-pane");
    const srcMax = src.scrollHeight - src.clientHeight;
    const paneMax = pane.scrollHeight - pane.clientHeight;
    if (srcMax <= 0 || paneMax <= 0) return;
    const kids = $("p-body").children;
    const points = [[0, 0]];
    if (blockLines.length && blockLines.length === kids.length) {
      if (!lineTops) lineTops = measureLines(blockLines);
      if (lineTops) {
        const padTop = parseFloat(getComputedStyle(src).paddingTop) || 0;
        const paneTop = pane.getBoundingClientRect().top - pane.scrollTop;
        for (let i = 0; i < kids.length; i++) {
          points.push([lineTops[i] - padTop, kids[i].getBoundingClientRect().top - paneTop - 24]);
        }
      }
    }
    points.push([srcMax, paneMax]);
    // Only points that go forward on both sides (a block at the very top or past the end is left
    // to its neighbours).
    const path = [];
    for (const [a, b] of points) {
      const last = path[path.length - 1];
      if (a < 0 || a > srcMax || b < 0 || b > paneMax) continue;
      if (last && (a <= last[0] || b < last[1])) continue;
      path.push([a, b]);
    }
    if (path[path.length - 1][0] !== srcMax) path.push([srcMax, paneMax]);
    const y = src.scrollTop;
    let i = 1;
    while (i < path.length - 1 && path[i][0] < y) i++;
    const [a0, b0] = path[i - 1];
    const [a1, b1] = path[i];
    const t = a1 > a0 ? Math.min(1, Math.max(0, (y - a0) / (a1 - a0))) : 1;
    pane.scrollTop = b0 + t * (b1 - b0);
  }

  $("body").addEventListener("scroll", syncPreview);
  // A picture or video's size is known once it loads.
  $("p-body").addEventListener("load", syncPreview, true);
  $("p-body").addEventListener("loadedmetadata", syncPreview, true);
  window.addEventListener("resize", () => {
    lineTops = null;
    syncPreview();
  });

  // ---------------------------------------------------------------- pasting text

  $("body").addEventListener("paste", (e) => {
    const files = [...(e.clipboardData?.files || [])].filter(isMedia);
    if (files.length) {
      e.preventDefault();
      files.forEach((f) => uploadFile(f));
      return;
    }
    // A whole post pasted into an empty one: its "# Title" line becomes the title.
    const text = e.clipboardData?.getData("text/plain") || "";
    const m = /^# +(.+)\r?\n/.exec(text);
    if (m && !$("body").value.trim() && !$("title").value.trim()) {
      e.preventDefault();
      $("title").value = m[1].trim();
      $("title").dispatchEvent(new Event("input"));
      insertText(text.slice(m[0].length).replace(/^\s*\n/, ""));
    }
  });

  function insertText(text) {
    const ta = $("body");
    ta.focus();
    // execCommand keeps the textarea's undo history; setRangeText where it is gone.
    if (!document.execCommand || !document.execCommand("insertText", false, text)) {
      ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, "end");
      changed();
    }
  }

  // ---------------------------------------------------------------- images and videos

  function isMedia(f) {
    return /^(image|video)\//.test(f.type);
  }

  $("upload").addEventListener("click", () => $("file").click());
  $("file").addEventListener("change", () => {
    [...$("file").files].forEach((f) => uploadFile(f));
    $("file").value = "";
  });

  const pane = $("src-pane");
  let dragDepth = 0;
  pane.addEventListener("dragenter", (e) => {
    if (![...(e.dataTransfer?.types || [])].includes("Files")) return;
    dragDepth++;
    $("drop").hidden = false;
  });
  pane.addEventListener("dragleave", () => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (!dragDepth) $("drop").hidden = true;
  });
  pane.addEventListener("dragover", (e) => {
    if ([...(e.dataTransfer?.types || [])].includes("Files")) e.preventDefault();
  });
  pane.addEventListener("drop", (e) => {
    dragDepth = 0;
    $("drop").hidden = true;
    const files = [...(e.dataTransfer?.files || [])].filter(isMedia);
    if (!files.length) return;
    e.preventDefault();
    // Put them where they were dropped.
    const ta = $("body");
    if (document.caretPositionFromPoint) {
      const pos = document.caretPositionFromPoint(e.clientX, e.clientY);
      if (pos && pos.offsetNode === ta) ta.setSelectionRange(pos.offset, pos.offset);
    }
    files.forEach((f) => uploadFile(f));
  });

  /** PNG/JPEG/BMP screenshots as WebP (and at most 2400 px wide), when that is smaller. */
  async function shrink(file) {
    if (!/^image\/(png|jpeg|bmp)$/.test(file.type) || !window.createImageBitmap) return file;
    try {
      const bmp = await createImageBitmap(file);
      const scale = Math.min(1, WEBP_MAX_WIDTH / bmp.width);
      const w = Math.round(bmp.width * scale);
      const h = Math.round(bmp.height * scale);
      const canvas = document.createElement("canvas");
      canvas.width = w;
      canvas.height = h;
      canvas.getContext("2d").drawImage(bmp, 0, 0, w, h);
      bmp.close?.();
      const blob = await new Promise((res) => canvas.toBlob(res, "image/webp", 0.9));
      if (!blob || blob.type !== "image/webp") return file;
      return blob.size < file.size || scale < 1 ? blob : file;
    } catch {
      return file;
    }
  }

  function sizeText(n) {
    return n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`;
  }

  /** Puts `text` at the cursor on lines of its own (a blank line before and after). */
  function insertBlock(text) {
    const ta = $("body");
    const before = ta.value.slice(0, ta.selectionStart);
    const after = ta.value.slice(ta.selectionEnd);
    const pre = !before || before.endsWith("\n\n") ? "" : before.endsWith("\n") ? "\n" : "\n\n";
    const post_ = after.startsWith("\n\n") ? "" : after.startsWith("\n") ? "\n" : "\n\n";
    insertText(pre + text + post_);
  }

  /** Replaces the first `from` in the Markdown with `to`, keeping the cursor where it was. */
  function replaceInBody(from, to) {
    const ta = $("body");
    const at = ta.value.indexOf(from);
    if (at < 0) return false;
    const { selectionStart: s, selectionEnd: e } = ta;
    const shift = (pos) => (pos > at ? Math.max(at, pos + to.length - from.length) : pos);
    ta.value = ta.value.slice(0, at) + to + ta.value.slice(at + from.length);
    ta.setSelectionRange(shift(s), shift(e));
    changed();
    return true;
  }

  async function uploadFile(file) {
    if (!post) return;
    const n = ++uploadSeq;
    const name = file.name || (file.type.startsWith("video/") ? "video" : "image");
    const holder = `<!-- Uploading ${name.replace(/--/g, "-")} (${n})… -->`;
    insertBlock(holder);

    const li = document.createElement("li");
    const label = document.createElement("span");
    label.className = "name";
    label.textContent = `Uploading ${name}`;
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "Cancel";
    const bar = document.createElement("span");
    bar.className = "bar";
    const fill = document.createElement("span");
    bar.append(fill);
    li.append(label, cancel, bar);
    $("uploads").append(li);
    let xhr = null;
    let cancelled = false;
    cancel.addEventListener("click", () => {
      cancelled = true;
      if (xhr) xhr.abort();
    });

    try {
      const blob = await shrink(file);
      if (cancelled) throw new BrawlAuth.ApiError(0, "aborted", "Upload cancelled.");
      if (blob.size > MAX_UPLOAD) {
        throw new BrawlAuth.ApiError(413, "too_large", `${name} is ${sizeText(blob.size)}; a file is at most 95 MB. Shorten or compress it first.`);
      }
      label.textContent = `Uploading ${name} (${sizeText(blob.size)})`;
      const r = await BrawlAuth.upload("/v1/blog/media", blob, {
        headers: { "X-File-Name": encodeURIComponent(name) },
        onProgress: (sent, total) => (fill.style.width = `${Math.round((100 * sent) / Math.max(1, total))}%`),
        xhr: (x) => (xhr = x),
      });
      if (!replaceInBody(holder, `![](${r.url})`)) insertBlock(`![](${r.url})`);
      const smaller = blob !== file ? ` (${sizeText(file.size)} → ${sizeText(blob.size)} WebP)` : ` (${sizeText(r.bytes)})`;
      toast(`${r.kind === "video" ? "Video" : "Image"} added${smaller}. Type a caption between the [ ].`);
    } catch (e) {
      replaceInBody(holder + "\n\n", "") || replaceInBody(holder, "");
      if (e.code !== "aborted") toast(`${name}: ${e.message}`, true);
    } finally {
      li.remove();
    }
  }

  start();
})();
