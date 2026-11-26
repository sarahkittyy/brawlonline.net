import { describe, expect, it } from "vitest";

import { changelogHtml, isNewChangelog } from "./changelog_html";

const BASE = "https://brawlonline.net";

describe("changelogHtml", () => {
  it("keeps the renderer's own output, with addresses on the accounts server", () => {
    const html =
      '<h2 id="launcher-0-2-9">Launcher 0.2.9</h2>\n<ul>\n<li>Fixed <strong>it</strong></li>\n</ul>\n' +
      '<figure><img src="/blog/media/0123456789abcdef0123.webp" alt="The menu" loading="lazy"><figcaption>The menu</figcaption></figure>\n' +
      '<figure><video src="/blog/media/0123456789abcdef0123.mp4" controls preload="metadata" playsinline></video></figure>';
    expect(changelogHtml(html, BASE)).toBe(
      "<h2>Launcher 0.2.9</h2>\n<ul>\n<li>Fixed <strong>it</strong></li>\n</ul>\n" +
        '<figure><img src="https://brawlonline.net/blog/media/0123456789abcdef0123.webp" alt="The menu"><figcaption>The menu</figcaption></figure>\n' +
        '<figure><video src="https://brawlonline.net/blog/media/0123456789abcdef0123.mp4" controls="" preload="metadata" playsinline=""></video></figure>',
    );
  });

  it("keeps a mention's player and drops everything else on a span", () => {
    const uid = "24a184c8-608e-46b9-9e7b-d26d805c545c";
    const out = changelogHtml(
      `<p>ty <span class="mention" data-uid="${uid}" data-code="SARA#694" data-discord="1">@sarahkitty</span> ` +
        '<span class="mention" data-discord="61305312321208320">@milla</span> ' +
        '<span class="mention" data-uid="nope" onclick="evil()">@x</span> <span data-uid="' +
        uid +
        '">@y</span></p>',
      BASE,
    );
    expect(out).toBe(
      `<p>ty <span data-mention="${uid}">@sarahkitty</span> <span>@milla</span> <span>@x</span> <span>@y</span></p>`,
    );
  });

  it("opens links in the browser and keeps only openable ones", () => {
    const out = changelogHtml(
      '<p><a href="https://example.com/x" onclick="evil()">site</a> <a href="/blog/post">post</a> ' +
        '<a href="javascript:alert(1)">bad</a> <sup><a href="#1">1</a></sup></p>',
      BASE,
    );
    expect(out).toBe(
      '<p><a href="https://example.com/x" target="_blank" rel="noopener noreferrer" title="https://example.com/x">site</a> ' +
        '<a href="https://brawlonline.net/blog/post" target="_blank" rel="noopener noreferrer" title="https://brawlonline.net/blog/post">post</a> ' +
        "bad <sup>1</sup></p>",
    );
  });

  it("drops anything the renderer never writes", () => {
    const out = changelogHtml(
      '<p onmouseover="x()">a<script>evil()</script><img src="https://elsewhere.example/a.png"><iframe src="/x"></iframe>' +
        '<marquee>b</marquee><input type="text"><input type="checkbox" checked></p><style>p{}</style>',
      BASE,
    );
    expect(out).toBe('<p>ab<input type="checkbox" disabled="" checked=""></p>');
  });

  it("keeps a table cell's alignment only", () => {
    expect(
      changelogHtml('<table><tr><td style="text-align: center">x</td><td style="color:red">y</td></tr></table>', BASE),
    ).toBe('<table><tbody><tr><td style="text-align: center">x</td><td>y</td></tr></tbody></table>');
  });
});

describe("isNewChangelog", () => {
  const built = "2026-11-26T12:00:00Z";

  it("shows one published before this build and after the last shown", () => {
    expect(isNewChangelog("2026-11-26T10:00:00Z", undefined, built)).toBe(true);
    expect(isNewChangelog("2026-11-26T10:00:00Z", "2026-11-20T10:00:00Z", built)).toBe(true);
  });

  it("never shows the same one twice, nor an older one", () => {
    expect(isNewChangelog("2026-11-26T10:00:00Z", "2026-11-26T10:00:00Z", built)).toBe(false);
    expect(isNewChangelog("2026-11-20T10:00:00Z", "2026-11-26T10:00:00Z", built)).toBe(false);
  });

  it("waits for its release when published after this launcher was built", () => {
    expect(isNewChangelog("2026-11-26T13:00:00Z", undefined, built)).toBe(false);
  });
});
