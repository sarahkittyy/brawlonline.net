//! The blog (docs/design/blog.md): the editor's API (admins only), the pages accounts renders and
//! the uploaded images and videos, against a real Postgres.

use e2e::{Stack, StackOptions};
use serde_json::json;

const PW: &str = "correct horse battery";

async fn signup(s: &Stack, email: &str, name: &str) -> String {
    let (st, body) =
        s.post("/v1/auth/signup", None, json!({"email": email, "password": PW, "displayName": name})).await;
    assert_eq!(st, 201, "signup: {body}");
    body["sessionToken"].as_str().unwrap().to_string()
}

async fn page(s: &Stack, path: &str) -> (u16, String, reqwest::header::HeaderMap) {
    let resp = s.http.get(s.url(path)).send().await.unwrap();
    let status = resp.status().as_u16();
    let headers = resp.headers().clone();
    (status, resp.text().await.unwrap(), headers)
}

async fn upload(s: &Stack, token: &str, bytes: Vec<u8>) -> (u16, serde_json::Value) {
    let resp = s
        .http
        .put(s.url("/v1/blog/media"))
        .bearer_auth(token)
        .header("x-file-name", "clip%201.png")
        .body(bytes)
        .send()
        .await
        .unwrap();
    let status = resp.status().as_u16();
    (status, resp.json().await.unwrap_or(serde_json::Value::Null))
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn blog_posts_pages_and_media() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let admin = signup(&s, "sarah@x.test", "sarah").await;
    let player = signup(&s, "player@x.test", "player").await;
    sqlx::query("UPDATE users SET role = 'admin' WHERE email = 'sarah@x.test'").execute(&s.pool).await.unwrap();

    // Only admins: not logged in 401, a player 403.
    let (st, _) = s.get("/v1/blog/posts", None).await;
    assert_eq!(st, 401);
    for (st, body) in [
        s.get("/v1/blog/posts", Some(&player)).await,
        s.post("/v1/blog/posts", Some(&player), json!({})).await,
        s.post("/v1/blog/render", Some(&player), json!({"markdown": "x"})).await,
        upload(&s, &player, b"\x89PNG\r\n\x1a\nabc".to_vec()).await,
    ] {
        assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_admin")), "{body}");
    }

    // Nothing yet.
    let (st, html, headers) = page(&s, "/blog").await;
    assert_eq!(st, 200);
    assert!(html.contains("No posts yet."));
    assert!(headers["content-security-policy"].to_str().unwrap().contains("media-src 'self'"));
    // No post, no "seen" cookie, and nothing for the home page's badge.
    assert!(headers.get("set-cookie").is_none());
    let (st, latest) = s.get("/v1/blog/latest", None).await;
    assert_eq!((st, &latest["latest"]), (200, &serde_json::Value::Null), "{latest}");

    // A new draft, then the progress report's start in it.
    let (st, post) = s.post("/v1/blog/posts", Some(&admin), json!({})).await;
    assert_eq!(st, 201, "{post}");
    assert_eq!(post["slug"], "untitled");
    assert_eq!(post["status"], "draft");
    let id = post["id"].as_i64().unwrap();
    let (st, second) = s.post("/v1/blog/posts", Some(&admin), json!({})).await;
    assert_eq!((st, second["slug"].as_str()), (201, Some("untitled-2")));

    // An image and a video (stored once per content, typed by their bytes).
    let png = b"\x89PNG\r\n\x1a\n-not-really-a-png-".to_vec();
    let (st, img) = upload(&s, &admin, png.clone()).await;
    assert_eq!(st, 200, "{img}");
    assert_eq!(img["kind"], "image");
    let img_url = img["url"].as_str().unwrap().to_string();
    assert!(img_url.starts_with("/blog/media/") && img_url.ends_with(".png"), "{img_url}");
    let (_, again) = upload(&s, &admin, png.clone()).await;
    assert_eq!(again["url"], img["url"]);
    let mut mp4 = b"\0\0\0\x20ftypisom".to_vec();
    mp4.extend(std::iter::repeat_n(7u8, 5000));
    let (st, vid) = upload(&s, &admin, mp4.clone()).await;
    assert_eq!((st, vid["kind"].as_str()), (200, Some("video")), "{vid}");
    let vid_url = vid["url"].as_str().unwrap().to_string();
    let (st, body) = upload(&s, &admin, b"<svg onload=alert(1)>".to_vec()).await;
    assert_eq!((st, body["error"]["code"].as_str()), (415, Some("unsupported_type")));
    let (st, body) = upload(&s, &admin, vec![]).await;
    assert_eq!(st, 400, "{body}");
    let stored: i64 = sqlx::query_scalar("SELECT count(*) FROM blog_media").fetch_one(&s.pool).await.unwrap();
    assert_eq!(stored, 2);

    // The files: whole, and a byte range (video seeking).
    let resp = s.http.get(s.url(&img_url)).send().await.unwrap();
    assert_eq!(resp.status().as_u16(), 200);
    assert_eq!(resp.headers()["content-type"], "image/png");
    assert!(resp.headers()["cache-control"].to_str().unwrap().contains("immutable"));
    assert_eq!(resp.bytes().await.unwrap().as_ref(), png.as_slice());
    let resp = s.http.get(s.url(&vid_url)).header("range", "bytes=100-199").send().await.unwrap();
    assert_eq!(resp.status().as_u16(), 206);
    assert_eq!(resp.headers()["content-type"], "video/mp4");
    assert_eq!(resp.bytes().await.unwrap().as_ref(), &mp4[100..200]);
    for bad in ["/blog/media/0123456789abcdef0123.png", "/blog/media/..%2F..%2Fsecret.png", "/blog/media/x.svg"] {
        let (st, _, _) = page(&s, bad).await;
        assert_eq!(st, 404, "{bad}");
    }

    // An ffmpeg 8 MP4 (a movie timescale Firefox freezes on) is stored with ffmpeg's usual 1000,
    // and the same upload again is the same file.
    let mvhd = [&[0u8; 12][..], &47_952_000u32.to_be_bytes(), &869_600_000u32.to_be_bytes(), &[0u8; 80]].concat();
    let moov = [&116u32.to_be_bytes()[..], b"moov", &108u32.to_be_bytes(), b"mvhd", &mvhd].concat();
    let ffmpeg8 = [&b"\0\0\0\x10ftypisom\0\0\x02\0\0\0\0\x6cmdat"[..], &[9u8; 100], &moov].concat();
    let (st, fixed) = upload(&s, &admin, ffmpeg8.clone()).await;
    assert_eq!((st, fixed["kind"].as_str()), (200, Some("video")), "{fixed}");
    let mut expected = ffmpeg8.clone();
    expected[124 + 16 + 12..124 + 16 + 20]
        .copy_from_slice(&[&1000u32.to_be_bytes()[..], &18_135u32.to_be_bytes()].concat());
    let resp = s.http.get(s.url(fixed["url"].as_str().unwrap())).send().await.unwrap();
    assert_eq!(resp.bytes().await.unwrap().as_ref(), expected.as_slice());
    let (_, again) = upload(&s, &admin, ffmpeg8).await;
    assert_eq!(again["url"], fixed["url"]);

    let body_md = format!(
        "Welcome to the very first progress report for **Brawl Online**!\n\n![The waiting room]({img_url})\n\n## New Ways to Play\n\nDoubles queue:\n- **Solo:** jump in\n- **Duo:** team up\n\n![]({vid_url})\n\n<script>alert(1)</script>\n"
    );
    let save = |slug: &str, expected: &serde_json::Value| {
        json!({
            "slug": slug,
            "title": "Progress Report #1: 100,000+ Matches!",
            "summary": "",
            "author": "AlephZero",
            "body": body_md,
            "expectedUpdatedAt": expected,
        })
    };
    let (st, body) = s.put(&format!("/v1/blog/posts/{id}"), Some(&admin), save("Bad Slug", &post["updatedAt"])).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("bad_slug")));
    let (st, body) = s.put(&format!("/v1/blog/posts/{id}"), Some(&admin), save("untitled-2", &post["updatedAt"])).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("slug_taken")));
    let (st, saved) =
        s.put(&format!("/v1/blog/posts/{id}"), Some(&admin), save("progress-report-1", &post["updatedAt"])).await;
    assert_eq!(st, 200, "{saved}");
    assert_eq!(saved["slug"], "progress-report-1");
    // A save over a newer version (another tab) is refused.
    let (st, body) =
        s.put(&format!("/v1/blog/posts/{id}"), Some(&admin), save("progress-report-1", &post["updatedAt"])).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("changed_elsewhere")));

    // The preview renders as the page will.
    let (st, r) = s.post("/v1/blog/render", Some(&admin), json!({"markdown": body_md})).await;
    assert_eq!(st, 200);
    let html = r["html"].as_str().unwrap();
    assert!(html.contains("<figcaption>The waiting room</figcaption>"), "{html}");
    assert!(html.contains("<video src=\""), "{html}");
    assert!(html.contains("&lt;script&gt;") && !html.contains("<script>"), "{html}");
    // Each top-level element's source line, for the editor's scrolling: ascending, from line 0.
    let blocks: Vec<u64> = r["blocks"].as_array().unwrap().iter().map(|b| b.as_u64().unwrap()).collect();
    assert_eq!(blocks.first(), Some(&0), "{r}");
    assert!(blocks.windows(2).all(|w| w[0] < w[1]), "{r}");

    // A draft: not on the index, 404 without its preview link, readable (noindex) with it.
    let (_, html, _) = page(&s, "/blog").await;
    assert!(html.contains("No posts yet."));
    let (st, _, _) = page(&s, "/blog/progress-report-1").await;
    assert_eq!(st, 404);
    let preview = saved["viewUrl"].as_str().unwrap();
    assert!(preview.starts_with("/blog/progress-report-1?preview="), "{preview}");
    let (st, html, headers) = page(&s, preview).await;
    assert_eq!(st, 200);
    assert!(html.contains("noindex") && html.contains("Draft preview"));
    assert_eq!(headers["cache-control"], "no-store");
    let (st, _, _) = page(&s, "/blog/progress-report-1?preview=wrong").await;
    assert_eq!(st, 404);

    // Publish: on the index (cover = its first image), its page public, the preview link gone.
    let (st, body) = s.post(&format!("/v1/blog/posts/{}/publish", second["id"]), Some(&admin), json!({})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("empty")), "an empty post can't be published");
    let (st, published) = s.post(&format!("/v1/blog/posts/{id}/publish"), Some(&admin), json!({})).await;
    assert_eq!(st, 200, "{published}");
    assert_eq!(published["status"], "published");
    assert_eq!(published["viewUrl"], "/blog/progress-report-1");
    let first_date = published["publishedAt"].clone();
    let (st, html, headers) = page(&s, "/blog").await;
    assert_eq!(st, 200);
    assert!(html.contains("class=\"hero\" href=\"/blog/progress-report-1\""), "{html}");
    assert!(html.contains(&format!("<img src=\"{img_url}\"")), "{html}");
    assert!(html.contains("PROGRESS REPORT #1") || html.contains("Progress Report #1"));
    assert!(html.contains("Welcome to the very first progress report for Brawl Online!"), "summary: {html}");
    assert!(!html.contains("Older posts"));
    assert_eq!(html.matches("<h1").count(), 1, "one h1 (the logo and Blog): {html}");
    assert!(html.contains("\"@type\":\"Blog\""), "{html}");
    assert!(html.contains(&format!("<link rel=\"canonical\" href=\"{}/blog\">", s.base)), "{html}");
    assert_eq!(headers["cache-control"], "public, max-age=60");
    // The home page's "new post" badge: the newest post, and the index's cookie with its date.
    let (st, latest) = s.get("/v1/blog/latest", None).await;
    assert_eq!(st, 200, "{latest}");
    assert_eq!(latest["latest"]["slug"], "progress-report-1");
    let first_secs = latest["latest"]["publishedAt"].as_i64().unwrap();
    assert_eq!(
        Some(first_secs),
        chrono::DateTime::parse_from_rfc3339(first_date.as_str().unwrap()).ok().map(|t| t.timestamp())
    );
    let cookie = headers["set-cookie"].to_str().unwrap();
    assert!(cookie.starts_with(&format!("bo_blog_seen={first_secs}; Path=/;")), "{cookie}");
    assert!(!cookie.contains("HttpOnly"), "the home page reads it: {cookie}");
    let (st, html, _) = page(&s, "/blog/progress-report-1").await;
    assert_eq!(st, 200);
    assert!(!html.contains("noindex"));
    assert!(html.contains("by AlephZero"));
    assert!(html.contains("<li><strong>Solo:</strong> jump in</li>"), "{html}");
    assert!(html.contains(&format!("og:image\" content=\"{}{img_url}", s.base)), "{html}");
    assert!(html.contains("\"@type\":\"BlogPosting\""));
    assert_eq!(html.matches("<h1").count(), 1, "one h1 (the title): {html}");
    assert!(html.contains(&format!("<link rel=\"canonical\" href=\"{}/blog/progress-report-1\">", s.base)));

    // The sitemap: the index and the published post (not the draft).
    let resp = s.http.get(s.url("/blog/sitemap.xml")).send().await.unwrap();
    assert_eq!(resp.status().as_u16(), 200);
    assert!(resp.headers()["content-type"].to_str().unwrap().starts_with("application/xml"));
    let xml = resp.text().await.unwrap();
    assert!(xml.contains(&format!("<loc>{}/blog</loc>", s.base)), "{xml}");
    assert!(xml.contains(&format!("<loc>{}/blog/progress-report-1</loc>", s.base)), "{xml}");
    assert!(xml.contains("<lastmod>"), "{xml}");
    assert!(!xml.contains("untitled-2"), "{xml}");
    let (st, _, headers) = page(&s, "/blog/").await;
    assert!(st == 200 || st == 308, "{st}"); // reqwest follows the redirect
    let _ = headers;

    // Unpublish and publish again: the date stays the first publish's.
    let (_, un) = s.post(&format!("/v1/blog/posts/{id}/unpublish"), Some(&admin), json!({})).await;
    assert_eq!(un["status"], "draft");
    let (st, _, _) = page(&s, "/blog/progress-report-1").await;
    assert_eq!(st, 404);
    let (_, again) = s.post(&format!("/v1/blog/posts/{id}/publish"), Some(&admin), json!({})).await;
    assert_eq!(again["publishedAt"], first_date);

    // A second published post: the newest is the hero, this one under "Older posts".
    let (st, _) = s
        .put(
            &format!("/v1/blog/posts/{}", second["id"]),
            Some(&admin),
            json!({"slug": "second", "title": "Second", "body": "Short.", "summary": "Mine"}),
        )
        .await;
    assert_eq!(st, 200);
    s.post(&format!("/v1/blog/posts/{}/publish", second["id"]), Some(&admin), json!({})).await;
    let (_, html, headers) = page(&s, "/blog").await;
    assert!(html.contains("class=\"hero\" href=\"/blog/second\""), "{html}");
    let (_, latest) = s.get("/v1/blog/latest", None).await;
    assert_eq!(latest["latest"]["slug"], "second");
    assert!(latest["latest"]["publishedAt"].as_i64().unwrap() >= first_secs);
    let cookie = headers["set-cookie"].to_str().unwrap().to_string();
    assert!(cookie.starts_with(&format!("bo_blog_seen={};", latest["latest"]["publishedAt"])), "{cookie}");
    assert!(html.contains("Older posts") && html.contains("href=\"/blog/progress-report-1\""), "{html}");
    assert!(html.contains("/assets/bg-poster.jpg"), "no image: the home page's still");

    // The editor's list: everything; delete.
    let (_, list) = s.get("/v1/blog/posts", Some(&admin)).await;
    assert_eq!(list.as_array().unwrap().len(), 2);
    let (st, _) = s.delete(&format!("/v1/blog/posts/{}", second["id"]), Some(&admin)).await;
    assert_eq!(st, 204);
    let (st, _, _) = page(&s, "/blog/second").await;
    assert_eq!(st, 404);

    // A long post goes through the blog's own body limit (the API's 16 KiB is for the rest).
    let long = "word ".repeat(40_000);
    let (st, body) = s
        .put(
            &format!("/v1/blog/posts/{id}"),
            Some(&admin),
            json!({"slug": "progress-report-1", "title": "T", "body": long}),
        )
        .await;
    assert_eq!(st, 200, "{body}");

    let _ = std::fs::remove_dir_all(std::env::temp_dir().join(format!("pp-e2e-blog-media-{}", s.db_name)));
    s.shutdown().await;
}

/// A post page with a User-Agent (and more headers), its status and HTML.
async fn read_as(s: &Stack, path: &str, ua: &str, extra: &[(&str, &str)]) -> (u16, String) {
    let mut req = s.http.get(s.url(path)).header("user-agent", ua);
    for (k, v) in extra {
        req = req.header(*k, *v);
    }
    let resp = req.send().await.unwrap();
    (resp.status().as_u16(), resp.text().await.unwrap())
}

/// Whether a post's byline ends with its views.
fn shows_views(html: &str) -> bool {
    html.contains(" view</p>") || html.contains(" views</p>")
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn blog_views() {
    const CHROME: &str = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36";
    const FIREFOX: &str = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:160.0) Gecko/20100101 Firefox/160.0";
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let admin = signup(&s, "sarah@x.test", "sarah").await;
    sqlx::query("UPDATE users SET role = 'admin' WHERE email = 'sarah@x.test'").execute(&s.pool).await.unwrap();
    let (_, post) = s.post("/v1/blog/posts", Some(&admin), json!({})).await;
    let id = post["id"].as_i64().unwrap();
    let (st, _) = s
        .put(&format!("/v1/blog/posts/{id}"), Some(&admin), json!({"slug": "news", "title": "News", "body": "Hello."}))
        .await;
    assert_eq!(st, 200);

    // A draft's preview link doesn't count.
    let token = post["viewUrl"].as_str().unwrap().split("preview=").nth(1).unwrap().to_string();
    let (st, html) = read_as(&s, &format!("/blog/news?preview={token}"), CHROME, &[]).await;
    assert_eq!(st, 200);
    assert!(!shows_views(&html), "{html}");
    s.post(&format!("/v1/blog/posts/{id}/publish"), Some(&admin), json!({})).await;

    // No browser, a crawler, a link preview, a prefetch: not counted, and no count shown at 0.
    let (_, html, _) = page(&s, "/blog/news").await;
    assert!(html.contains("1 min read") && !shows_views(&html), "{html}");
    for ua in ["Twitterbot/1.0", "Mozilla/5.0 (compatible; Discordbot/2.0; +https://discordapp.com)"] {
        let (st, html) = read_as(&s, "/blog/news", ua, &[]).await;
        assert_eq!(st, 200);
        assert!(!shows_views(&html), "{ua}: {html}");
    }
    read_as(&s, "/blog/news", CHROME, &[("sec-purpose", "prefetch")]).await;
    let head = s.http.head(s.url("/blog/news")).header("user-agent", CHROME).send().await.unwrap();
    assert_eq!(head.status().as_u16(), 200);

    // A reader counts once a day, however often they reload; another browser is another reader.
    let (_, html) = read_as(&s, "/blog/news", CHROME, &[]).await;
    assert!(html.contains("1 min read · 1 view</p>"), "{html}");
    let (_, html) = read_as(&s, "/blog/news", CHROME, &[]).await;
    assert!(html.contains("· 1 view</p>"), "{html}");
    let (_, html) = read_as(&s, "/blog/news", FIREFOX, &[]).await;
    assert!(html.contains("· 2 views</p>"), "{html}");

    // The index: after the date.
    let (_, html, _) = page(&s, "/blog").await;
    assert!(html.contains("<span class=\"views\"><span class=\"sep\"> · </span>2 views</span></span>"), "{html}");

    // The next day (UTC) the same reader counts again, and the earlier days' rows go.
    sqlx::query("UPDATE blog_post_view_seen SET day = day - 1").execute(&s.pool).await.unwrap();
    let (_, html) = read_as(&s, "/blog/news", CHROME, &[]).await;
    assert!(html.contains("· 3 views</p>"), "{html}");
    let rows: i64 = sqlx::query_scalar("SELECT count(*) FROM blog_post_view_seen").fetch_one(&s.pool).await.unwrap();
    assert_eq!(rows, 1);
    let views: i64 =
        sqlx::query_scalar("SELECT views FROM blog_posts WHERE id = $1").bind(id).fetch_one(&s.pool).await.unwrap();
    assert_eq!(views, 3);

    // Editing the post leaves its views alone.
    let (st, _) = s
        .put(&format!("/v1/blog/posts/{id}"), Some(&admin), json!({"slug": "news", "title": "News!", "body": "Hello."}))
        .await;
    assert_eq!(st, 200);
    let (_, html, _) = page(&s, "/blog/news").await;
    assert!(html.contains("· 3 views</p>"), "{html}");

    let _ = std::fs::remove_dir_all(std::env::temp_dir().join(format!("pp-e2e-blog-media-{}", s.db_name)));
    s.shutdown().await;
}
