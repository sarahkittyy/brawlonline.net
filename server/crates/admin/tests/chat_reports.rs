//! `admin chat-reports` lists the unhandled chat reports newest first, with the verified count and
//! each message's cleaned text; `admin chat-report-done <id>` takes one off the list. Runs the real
//! `admin` executable against a fresh database.

use accounts::store;
use common::db::{insert_chat_report, NewChatReport};
use serde_json::json;

async fn run(url: &str, args: &[&str]) -> (bool, String) {
    let out = tokio::process::Command::new(env!("CARGO_BIN_EXE_admin"))
        .args(args)
        .current_dir(std::env::temp_dir())
        .env("DATABASE_URL", url)
        .output()
        .await
        .unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    (out.status.success(), text)
}

#[tokio::test]
async fn list_and_handle_chat_reports() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-chat-a@example.test", "x", "alice").await.unwrap();
    let bob = store::create_user(&pool, "zz-chat-b@example.test", "x", "bob").await.unwrap();
    let (_, text) = run(&url, &["chat-reports"]).await;
    assert!(text.contains("no unhandled chat reports"), "{text}");

    for (reason, group) in [("first", "room-KFQB-1"), ("second", "mode.direct-x-2")] {
        let report = NewChatReport {
            reporter: alice.uid,
            reported: bob.uid,
            group_id: group.into(),
            reason: reason.into(),
            messages: json!([
                {"seq": 1, "text": "you are bad", "raw": "", "sig": "", "verified": true},
                {"seq": 2, "text": "i cheat", "raw": "", "sig": "", "verified": false},
            ]),
            verified: 1,
            total: 2,
        };
        insert_chat_report(&pool, &report).await.unwrap();
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    let (ok, text) = run(&url, &["chat-reports"]).await;
    assert!(ok, "{text}");
    let (first, second) = (text.find("room-KFQB-1").unwrap(), text.find("mode.direct-x-2").unwrap());
    assert!(second < first, "newest first: {text}");
    for want in ["reporter: - (alice)", "reported: - (bob)", "1 of 2 verified", "you are bad", "NOT SIGNED", "i cheat"]
    {
        assert!(text.contains(want), "{want:?} missing: {text}");
    }
    let id: i64 =
        sqlx::query_scalar("SELECT id FROM chat_reports WHERE reason = 'first'").fetch_one(&pool).await.unwrap();
    let (ok, text) = run(&url, &["chat-report-done", &id.to_string()]).await;
    assert!(ok && text.contains("handled"), "{text}");
    let (_, text) = run(&url, &["chat-reports"]).await;
    assert!(!text.contains("room-KFQB-1") && text.contains("mode.direct-x-2"), "{text}");
    // Twice, or an unknown id: an error.
    let (ok, _) = run(&url, &["chat-report-done", &id.to_string()]).await;
    assert!(!ok);
    let audit: i64 = sqlx::query_scalar("SELECT count(*) FROM audit_log WHERE action = 'chat_report.done'")
        .fetch_one(&pool)
        .await
        .unwrap();
    assert_eq!(audit, 1);
    pool.close().await;
    e2e::drop_db(&name).await;
}
