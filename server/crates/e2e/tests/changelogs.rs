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
    // The Markdown too, as stored (the picture under its stored name), for the Discord bot.
    assert_eq!(c["markdown"].as_str(), Some(md.replace("0123456789abcdef0123.png", name).as_str()));

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

    // The board's first push with patch notes: published once that push has deployed pp-admin,
    // dated just before it (so the launchers it builds show them); never ahead, at most 2 days back.
    let before_push = chrono::Utc::now() - chrono::Duration::minutes(9);
    let p3 =
        changelog::publish_at(&s.pool, &media_dir, "# Launcher 0.3.1", &[], false, Some(before_push)).await.unwrap();
    assert!((p3.published_at - before_push).num_milliseconds().abs() < 5, "{} vs {before_push}", p3.published_at);
    // Latest is by date: here the one published a moment ago is newer (on prod it is the only one).
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(p2.id), "{latest}");
    assert!(changelog::list(&s.pool).await.unwrap().iter().any(|r| r.id == p3.id && r.published_at == p3.published_at));
    let ahead = chrono::Utc::now() + chrono::Duration::minutes(10);
    assert!(err(changelog::publish_at(&s.pool, &media_dir, "x", &[], false, Some(ahead)).await).contains("future"));
    let old = chrono::Utc::now() - chrono::Duration::days(3);
    assert!(err(changelog::publish_at(&s.pool, &media_dir, "x", &[], false, Some(old)).await).contains("2 days"));

    let _ = std::fs::remove_dir_all(&media_dir);
}

/// The launcher's feed (`GET /v1/changelogs`): newest first in pages, the staging-only one ahead on
/// the first page, and `before` (a launcher's build time) leaving out the ones for a release to come.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn changelogs_feed_pages() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let media_dir = std::env::temp_dir().join(format!("pp-e2e-changelog-feed-{}", std::process::id()));
    let ids = |v: &serde_json::Value| -> Vec<i64> {
        v["changelogs"].as_array().unwrap().iter().map(|c| c["id"].as_i64().unwrap()).collect()
    };

    let (st, page) = s.get("/v1/changelogs", None).await;
    assert_eq!(st, 200, "{page}");
    assert!(ids(&page).is_empty() && page["next"].is_null(), "{page}");

    let now = chrono::Utc::now();
    let mut published = Vec::new();
    for (i, mins) in [50, 40, 30, 20, 10].into_iter().enumerate() {
        let md = format!("# Launcher 0.2.{}", 5 + i);
        let at = Some(now - chrono::Duration::minutes(mins));
        published.push(changelog::publish_at(&s.pool, &media_dir, &md, &[], false, at).await.unwrap());
    }
    let id = |i: usize| published[i].id;

    // Pages of 2, newest first, until `next` is null.
    let (_, p1) = s.get("/v1/changelogs?limit=2", None).await;
    assert_eq!(ids(&p1), vec![id(4), id(3)], "{p1}");
    assert!(p1["changelogs"][0]["html"].as_str().unwrap().contains("Launcher 0.2.9"), "{p1}");
    assert_eq!(p1["changelogs"][0]["markdown"], "# Launcher 0.2.9", "{p1}");
    let next = p1["next"].as_str().unwrap();
    let (_, p2) = s.get(&format!("/v1/changelogs?limit=2&after={next}"), None).await;
    assert_eq!(ids(&p2), vec![id(2), id(1)], "{p2}");
    let next = p2["next"].as_str().unwrap();
    let (_, p3) = s.get(&format!("/v1/changelogs?limit=2&after={next}"), None).await;
    assert_eq!(ids(&p3), vec![id(0)], "{p3}");
    assert!(p3["next"].is_null(), "{p3}");

    // A launcher built 15 minutes ago: the newest is for a release to come, so its feed starts
    // below it (one updated from 0.2.7 sees 0.2.8's notes first even once 0.2.9's are out).
    let built = (now - chrono::Duration::minutes(15)).to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
    let (_, b) = s.get(&format!("/v1/changelogs?limit=2&before={built}"), None).await;
    assert_eq!(ids(&b), vec![id(3), id(2)], "{b}");

    // Staging's every-start one leads the first page only, whatever `before` says.
    let e = changelog::publish(&s.pool, &media_dir, "draft", &[], true).await.unwrap();
    let (_, b) = s.get(&format!("/v1/changelogs?limit=2&before={built}"), None).await;
    assert_eq!(ids(&b), vec![e.id, id(3), id(2)], "{b}");
    assert_eq!(b["changelogs"][0]["everyStart"], true);
    let (_, b2) = s.get(&format!("/v1/changelogs?limit=2&after={}", b["next"].as_str().unwrap()), None).await;
    assert_eq!(ids(&b2), vec![id(1), id(0)], "{b2}");

    // Refused: a cursor it didn't give out, a time that isn't one.
    let (st, _) = s.get("/v1/changelogs?after=nope", None).await;
    assert_eq!(st, 400);
    let (st, _) = s.get("/v1/changelogs?before=yesterday", None).await;
    assert_eq!(st, 400);

    let _ = std::fs::remove_dir_all(&media_dir);
}

/// EmissaryRecomp's patch notes (`pp-admin changelog publish --emissary v0.7.2`): in the feed only
/// for a launcher that asks (`emissary=1`, its Play uses the recomp), with their product and
/// version, between the launcher's by date, never held back by `before`; never in `/latest`.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn emissary_changelogs_are_asked_for() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let media_dir = std::env::temp_dir().join(format!("pp-e2e-changelog-emissary-{}", std::process::id()));
    let ids = |v: &serde_json::Value| -> Vec<i64> {
        v["changelogs"].as_array().unwrap().iter().map(|c| c["id"].as_i64().unwrap()).collect()
    };
    let now = chrono::Utc::now();
    let at = |mins: i64| Some(now - chrono::Duration::minutes(mins));
    let err = |r: anyhow::Result<changelog::Published>| r.unwrap_err().to_string();
    let l1 = changelog::publish_at(&s.pool, &media_dir, "# Launcher 0.2.10", &[], false, at(30)).await.unwrap();
    let e1 = changelog::publish_for(&s.pool, &media_dir, "# Emissary", &[], false, at(20), Some("v0.7.2"))
        .await
        .unwrap();
    let l2 = changelog::publish_at(&s.pool, &media_dir, "# Launcher 0.2.11", &[], false, at(10)).await.unwrap();
    let e2 = changelog::publish_for(&s.pool, &media_dir, "# Emissary again", &[], false, None, Some("v0.7.3"))
        .await
        .unwrap();
    assert!(err(changelog::publish_for(&s.pool, &media_dir, "x", &[], false, None, Some("0.7.3")).await)
        .contains("v0.7.2"));

    let (_, plain) = s.get("/v1/changelogs?limit=5", None).await;
    assert_eq!(ids(&plain), vec![l2.id, l1.id], "{plain}");
    assert_eq!(plain["changelogs"][0]["product"], "launcher");
    assert!(plain["changelogs"][0].get("version").is_none(), "{plain}");
    let (_, both) = s.get("/v1/changelogs?limit=2&emissary=1", None).await;
    assert_eq!(ids(&both), vec![e2.id, l2.id], "{both}");
    assert_eq!(both["changelogs"][0]["product"], "emissary");
    assert_eq!(both["changelogs"][0]["version"], "v0.7.3");
    let (_, more) = s.get(&format!("/v1/changelogs?limit=2&emissary=1&after={}", both["next"].as_str().unwrap()), None).await;
    assert_eq!(ids(&more), vec![e1.id, l1.id], "{more}");
    // A launcher built 15 minutes ago: the launcher's newer notes wait for their release; the
    // recomp's are out already.
    let built = (now - chrono::Duration::minutes(15)).to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
    let (_, b) = s.get(&format!("/v1/changelogs?limit=5&emissary=1&before={built}"), None).await;
    assert_eq!(ids(&b), vec![e2.id, e1.id, l1.id], "{b}");
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(l2.id), "{latest}");

    // Staging's every-start ones, one per product.
    let el = changelog::publish(&s.pool, &media_dir, "draft", &[], true).await.unwrap();
    let ee = changelog::publish_for(&s.pool, &media_dir, "draft", &[], true, None, Some("v0.7.4")).await.unwrap();
    let ee2 = changelog::publish_for(&s.pool, &media_dir, "draft 2", &[], true, None, Some("v0.7.4")).await.unwrap();
    let (_, plain) = s.get("/v1/changelogs?limit=1", None).await;
    assert_eq!(ids(&plain), vec![el.id, l2.id], "{plain}");
    let (_, both) = s.get("/v1/changelogs?limit=1&emissary=1", None).await;
    assert_eq!(ids(&both), vec![ee2.id, el.id, e2.id], "{both}");
    assert!(!ids(&both).contains(&ee.id));

    let _ = std::fs::remove_dir_all(&media_dir);
}
