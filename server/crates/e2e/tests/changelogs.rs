//! The launcher's changelogs (docs/design/changelog.md): the editor's API (admins only) and the
//! newest published one that updated launchers show, against a real Postgres.

use e2e::{Stack, StackOptions};
use serde_json::json;

const PW: &str = "correct horse battery";

async fn signup(s: &Stack, email: &str, name: &str) -> String {
    let (st, body) =
        s.post("/v1/auth/signup", None, json!({"email": email, "password": PW, "displayName": name})).await;
    assert_eq!(st, 201, "signup: {body}");
    body["sessionToken"].as_str().unwrap().to_string()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn changelogs_drafts_publish_and_latest() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let admin = signup(&s, "sarah@x.test", "sarah").await;
    let player = signup(&s, "player@x.test", "player").await;
    sqlx::query("UPDATE users SET role = 'admin' WHERE email = 'sarah@x.test'").execute(&s.pool).await.unwrap();

    // Only admins write them: not logged in 401, a player 403.
    let (st, _) = s.get("/v1/changelogs", None).await;
    assert_eq!(st, 401);
    for (st, body) in [
        s.get("/v1/changelogs", Some(&player)).await,
        s.post("/v1/changelogs", Some(&player), json!({})).await,
        s.put("/v1/changelogs/1", Some(&player), json!({"body": "x"})).await,
        s.post("/v1/changelogs/1/publish", Some(&player), json!({})).await,
        s.delete("/v1/changelogs/1", Some(&player)).await,
    ] {
        assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_admin")), "{body}");
    }

    // Anyone (a launcher, logged in or not) reads the newest; none yet.
    let (st, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(st, 200, "{latest}");
    assert!(latest["changelog"].is_null(), "{latest}");

    // A draft: saved, but not what launchers get.
    let (st, draft) = s.post("/v1/changelogs", Some(&admin), json!({})).await;
    assert_eq!(st, 201, "{draft}");
    assert_eq!(draft["status"], "draft");
    let id = draft["id"].as_i64().unwrap();
    let (st, body) = s.post(&format!("/v1/changelogs/{id}/publish"), Some(&admin), json!({})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("empty")), "an empty one can't be published");
    let text = "# Launcher 0.2.9\n\n- Fixed it\n\n![The new menu](/blog/media/0123456789abcdef0123.webp)\n\n<b>raw</b>";
    let (st, saved) = s
        .put(
            &format!("/v1/changelogs/{id}"),
            Some(&admin),
            json!({"body": text, "expectedUpdatedAt": draft["updatedAt"]}),
        )
        .await;
    assert_eq!(st, 200, "{saved}");
    assert_eq!(saved["title"], "Launcher 0.2.9");
    let (st, body) = s
        .put(
            &format!("/v1/changelogs/{id}"),
            Some(&admin),
            json!({"body": "old tab", "expectedUpdatedAt": draft["updatedAt"]}),
        )
        .await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("changed_elsewhere")), "{body}");
    let (st, body) = s.put(&format!("/v1/changelogs/{id}"), Some(&admin), json!({"body": "x".repeat(49 * 1024)})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (413, Some("too_large")), "{body}");
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert!(latest["changelog"].is_null(), "a draft is not served: {latest}");

    // Published: the launchers' changelog, rendered as the blog renders.
    let (st, published) = s.post(&format!("/v1/changelogs/{id}/publish"), Some(&admin), json!({})).await;
    assert_eq!(st, 200, "{published}");
    assert_eq!(published["status"], "published");
    let first_date = published["publishedAt"].clone();
    let (st, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(st, 200);
    let c = &latest["changelog"];
    assert_eq!(c["id"].as_i64(), Some(id));
    assert_eq!(c["publishedAt"], first_date);
    let html = c["html"].as_str().unwrap();
    assert!(html.contains("<h2 id=\"launcher-0-2-9\">Launcher 0.2.9</h2>"), "{html}");
    assert!(html.contains("<figure><img src=\"/blog/media/0123456789abcdef0123.webp\""), "{html}");
    assert!(html.contains("<figcaption>The new menu</figcaption>"), "{html}");
    assert!(html.contains("&lt;b&gt;raw&lt;/b&gt;") && !html.contains("<b>"), "raw HTML is text: {html}");

    // An edit keeps it the same changelog (launchers that showed it don't show it again).
    let (_, edited) = s
        .put(&format!("/v1/changelogs/{id}"), Some(&admin), json!({"body": "# Launcher 0.2.9\n\n- Fixed it twice"}))
        .await;
    assert_eq!(edited["publishedAt"], first_date);
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert!(latest["changelog"]["html"].as_str().unwrap().contains("Fixed it twice"));

    // A newer one is the latest; unpublishing it brings back the older one with its own date.
    let (_, second) = s.post("/v1/changelogs", Some(&admin), json!({"body": "# Launcher 0.3.0"})).await;
    let second_id = second["id"].as_i64().unwrap();
    s.post(&format!("/v1/changelogs/{second_id}/publish"), Some(&admin), json!({})).await;
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(second_id));
    let (_, list) = s.get("/v1/changelogs", Some(&admin)).await;
    let ids: Vec<i64> = list.as_array().unwrap().iter().map(|c| c["id"].as_i64().unwrap()).collect();
    assert_eq!(ids, vec![second_id, id], "newest published first");
    let (_, un) = s.post(&format!("/v1/changelogs/{second_id}/unpublish"), Some(&admin), json!({})).await;
    assert_eq!(un["status"], "draft");
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert_eq!(latest["changelog"]["id"].as_i64(), Some(id));
    assert_eq!(latest["changelog"]["publishedAt"], first_date);

    // Deleted: gone from the list and the latest.
    let (st, _) = s.delete(&format!("/v1/changelogs/{id}"), Some(&admin)).await;
    assert_eq!(st, 204);
    let (st, _) = s.get(&format!("/v1/changelogs/{id}"), Some(&admin)).await;
    assert_eq!(st, 404);
    let (_, latest) = s.get("/v1/changelogs/latest", None).await;
    assert!(latest["changelog"].is_null(), "{latest}");
}
