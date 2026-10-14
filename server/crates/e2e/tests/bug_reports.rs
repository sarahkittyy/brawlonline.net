//! End-to-end: bug reports from the launcher (`accounts::bug_reports`). The text with and without
//! a login, the logs zip once with its token (not twice, not with another token, not after it
//! came, not a non-zip, not over the size limit), and the per-IP rate limit.

use e2e::{Stack, StackOptions};
use serde_json::{json, Value};

/// The smallest zip there is: an empty archive (just the end of central directory record).
const EMPTY_ZIP: [u8; 22] = *b"PK\x05\x06\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

fn zip_of(len: usize) -> Vec<u8> {
    let mut z = b"PK\x03\x04".to_vec();
    z.resize(len, 7);
    z
}

async fn put_logs(s: &Stack, id: i64, token: &str, body: Vec<u8>) -> (u16, Value) {
    let resp = s
        .http
        .put(s.url(&format!("/v1/bug-reports/{id}/logs")))
        .header("x-upload-token", token)
        .header("content-type", "application/zip")
        .body(body)
        .send()
        .await
        .expect("http");
    let status = resp.status().as_u16();
    let text = resp.text().await.unwrap_or_default();
    (status, serde_json::from_str(&text).unwrap_or(Value::Null))
}

fn created(v: &Value) -> (i64, String) {
    (v["id"].as_i64().unwrap(), v["uploadToken"].as_str().unwrap().to_string())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn report_with_logs() {
    let s =
        Stack::start(StackOptions { bug_report_max_logs_bytes: Some(64 * 1024), ..Default::default() }).await.unwrap();
    let (alice, _) = s.create_player("alice@x.test", "alice", "ALIC").await;

    let body = json!({
        "description": "  The game froze\r\non the CSS\u{0}  ",
        "launcherVersion": "1.4.2",
        "platform": "win32 x64 (Windows 11)",
    });
    let (st, v) = s.post("/v1/bug-reports", Some(&alice), body.clone()).await;
    assert_eq!(st, 201, "{v}");
    let (id, token) = created(&v);
    let (uid, description, version, platform, logs): (Option<uuid::Uuid>, String, String, String, Option<i64>) =
        sqlx::query_as(
            "SELECT uid, description, launcher_version, platform, logs_bytes FROM bug_reports WHERE id = $1",
        )
        .bind(id)
        .fetch_one(&s.pool)
        .await
        .unwrap();
    assert!(uid.is_some());
    assert_eq!(description, "The game froze\non the CSS");
    assert_eq!((version.as_str(), platform.as_str(), logs), ("1.4.2", "win32 x64 (Windows 11)", None));

    // Another token, an unknown report, a non-zip, too large: refused, nothing kept.
    assert_eq!(put_logs(&s, id, "nope", zip_of(100)).await.0, 404);
    assert_eq!(put_logs(&s, id + 100, &token, zip_of(100)).await.0, 404);
    let (st, v) = put_logs(&s, id, &token, b"hello, not a zip".to_vec()).await;
    assert_eq!((st, v["error"]["code"].as_str()), (400, Some("not_a_zip")), "{v}");
    let (st, v) = put_logs(&s, id, &token, zip_of(64 * 1024 + 1)).await;
    assert_eq!((st, v["error"]["code"].as_str()), (413, Some("too_large")), "{v}");
    let leftovers = std::fs::read_dir(&s.bug_reports_dir).map(|d| d.count()).unwrap_or(0);
    assert_eq!(leftovers, 0, "refused uploads leave no files");

    // The zip: kept as bug-<id>.zip, once.
    let zip = zip_of(64 * 1024);
    assert_eq!(put_logs(&s, id, &token, zip.clone()).await.0, 204);
    assert_eq!(std::fs::read(s.bug_reports_dir.join(format!("bug-{id}.zip"))).unwrap(), zip);
    let logs: Option<i64> = sqlx::query_scalar("SELECT logs_bytes FROM bug_reports WHERE id = $1")
        .bind(id)
        .fetch_one(&s.pool)
        .await
        .unwrap();
    assert_eq!(logs, Some(64 * 1024));
    assert_eq!(put_logs(&s, id, &token, EMPTY_ZIP.to_vec()).await.0, 404, "one zip per report");

    // Logged out (or a dead session): still a report, without an account. An empty zip is a zip.
    for token in [None, Some("not-a-session")] {
        let (st, v) = s.post("/v1/bug-reports", token, json!({"description": "can't log in"})).await;
        assert_eq!(st, 201, "{v}");
        let (id2, upload) = created(&v);
        let uid: Option<uuid::Uuid> =
            sqlx::query_scalar("SELECT uid FROM bug_reports WHERE id = $1").bind(id2).fetch_one(&s.pool).await.unwrap();
        assert_eq!(uid, None);
        assert_eq!(put_logs(&s, id2, &upload, EMPTY_ZIP.to_vec()).await.0, 204);
    }

    // A zip after the upload window: refused.
    let (_, v) = s.post("/v1/bug-reports", None, json!({"description": "late"})).await;
    let (id3, upload) = created(&v);
    sqlx::query("UPDATE bug_reports SET created_at = now() - interval '2 hours' WHERE id = $1")
        .bind(id3)
        .execute(&s.pool)
        .await
        .unwrap();
    assert_eq!(put_logs(&s, id3, &upload, EMPTY_ZIP.to_vec()).await.0, 404);
    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn reports_are_rate_limited_per_ip() {
    let s = Stack::start(StackOptions { rate_limits: true, trust_proxy_headers: true, ..Default::default() })
        .await
        .unwrap();
    let post = |ip: &'static str| {
        let req =
            s.http.post(s.url("/v1/bug-reports")).header("x-forwarded-for", ip).json(&json!({"description": "x"}));
        async move { req.send().await.unwrap().status().as_u16() }
    };
    for _ in 0..5 {
        assert_eq!(post("203.0.113.7").await, 201);
    }
    assert_eq!(post("203.0.113.7").await, 429);
    assert_eq!(post("203.0.113.8").await, 201, "another address has its own count");
    s.shutdown().await;
}
