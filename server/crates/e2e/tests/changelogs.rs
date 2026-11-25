//! The launcher's changelogs (docs/design/changelog.md): `changelog::publish` (what `pp-admin
//! changelog publish` runs for the agent board) and the newest one updated launchers show, against
//! a real Postgres.

use accounts::changelog::{self, MediaFile};
use e2e::{Stack, StackOptions};

/// A 1x1 PNG.
const PNG: &[u8] = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0\x1f\x15\xc4\x89\0\0\0\rIDATx\x9cc\xf8\xff\xff?\0\x05\xfe\x02\xfe\xa7\x35\x81\x84\0\0\0\0IEND\xaeB`\x82";

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn changelogs_publish_and_latest() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let media_dir = std::env::temp_dir().join(format!("pp-e2e-changelog-media-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&media_dir);

    // None yet.
    let (st, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(st, 200, "{latest}");
    assert!(latest["changelog"].is_null(), "{latest}");

    // Refused: empty, a picture neither there nor sent, a file that isn't a picture.
    let err = |r: anyhow::Result<changelog::Published>| r.unwrap_err().to_string();
    assert!(err(changelog::publish(&s.pool, &media_dir, " \n", &[], false).await).contains("empty"));
    let missing = "![x](/blog/media/0123456789abcdef0123.png)";
    assert!(
        err(changelog::publish(&s.pool, &media_dir, missing, &[], false).await).contains("0123456789abcdef0123.png")
    );
    let bad = [MediaFile { name: "0123456789abcdef0123.png".into(), data: b"<svg/>".to_vec() }];
    assert!(err(changelog::publish(&s.pool, &media_dir, missing, &bad, false).await).contains("not a PNG"));

    // Published: the picture goes in under its content's name (the board's name for it was a
    // placeholder here), and the Markdown points at that.
    let md = "# Launcher 0.2.9\n\n- Fixed it\n\n![The new menu](/blog/media/0123456789abcdef0123.png)\n\n<b>raw</b>";
    let sent = [MediaFile { name: "0123456789abcdef0123.png".into(), data: PNG.to_vec() }];
    let p = changelog::publish(&s.pool, &media_dir, md, &sent, false).await.unwrap();
    assert_eq!(p.written.len(), 1);
    let name = &p.written[0];
    assert!(name.ends_with(".png") && name != "0123456789abcdef0123.png", "{name}");
    assert_eq!(std::fs::read(media_dir.join(name)).unwrap(), PNG);
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    let c = &latest["changelog"];
    assert_eq!(c["id"].as_i64(), Some(p.id));
    assert_eq!(c["everyStart"], false);
    let html = c["html"].as_str().unwrap();
    assert!(html.contains("<h2 id=\"launcher-0-2-9\">Launcher 0.2.9</h2>"), "{html}");
    assert!(html.contains(&format!("<figure><img src=\"/blog/media/{name}\" alt=\"The new menu\"")), "{html}");
    assert!(html.contains("&lt;b&gt;raw&lt;/b&gt;") && !html.contains("<b>"), "raw HTML is text: {html}");

    // The same picture again: already there, nothing written; a newer changelog is the latest.
    let md2 = format!("# Launcher 0.3.0\n\n![](/blog/media/{name})");
    let p2 = changelog::publish(&s.pool, &media_dir, &md2, &[], false).await.unwrap();
    assert!(p2.written.is_empty());
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(p2.id));

    // Staging's "show on staging": an every-start one comes first, and a new one replaces it.
    let e1 = changelog::publish(&s.pool, &media_dir, "draft one", &[], true).await.unwrap();
    let e2 = changelog::publish(&s.pool, &media_dir, "draft two", &[], true).await.unwrap();
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(e2.id));
    assert_eq!(latest["changelog"]["everyStart"], true);
    let ids: Vec<i64> = changelog::list(&s.pool).await.unwrap().iter().map(|r| r.id).collect();
    assert!(!ids.contains(&e1.id) && ids.contains(&e2.id), "{ids:?}");

    // Turned off: back to the newest real one.
    assert_eq!(changelog::clear_every_start(&s.pool).await.unwrap(), 1);
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(p2.id));
    assert_eq!(latest["changelog"]["everyStart"], false);

    let _ = std::fs::remove_dir_all(&media_dir);
}
