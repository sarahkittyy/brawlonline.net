//! `admin user rename IDENT NAME` changes a display name with the launcher's rule and audit-logs
//! the old and new name; a name the launcher would refuse changes nothing. Runs the real `admin`
//! executable against a fresh database.

use accounts::store;

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
async fn rename_a_user() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-rename@example.test", "x", "alice").await.unwrap();
    let display = || async {
        sqlx::query_scalar::<_, String>("SELECT display_name FROM users WHERE uid = $1")
            .bind(alice.uid)
            .fetch_one(&pool)
            .await
            .unwrap()
    };

    let (ok, text) = run(&url, &["user", "rename", "zz-rename@example.test", "Mr. Game&Watch"]).await;
    assert!(ok, "{text}");
    assert!(text.contains(r#"from "alice" to "Mr. Game&Watch""#), "{text}");
    assert_eq!(display().await, "Mr. Game&Watch");
    let detail: serde_json::Value =
        sqlx::query_scalar("SELECT detail FROM audit_log WHERE action = 'user.rename' AND target = $1")
            .bind(alice.uid.to_string())
            .fetch_one(&pool)
            .await
            .unwrap();
    assert_eq!(detail, serde_json::json!({"from": "alice", "to": "Mr. Game&Watch"}));

    // What the launcher refuses: too long, a character the font lacks, only spaces.
    for bad in ["Mr. Game & Watch", "a\\b", "漢字", "   "] {
        let (ok, text) = run(&url, &["user", "rename", &alice.uid.to_string(), bad]).await;
        assert!(!ok, "{bad:?} accepted: {text}");
        assert_eq!(display().await, "Mr. Game&Watch", "{bad:?}");
    }
    let audits: i64 = sqlx::query_scalar("SELECT count(*) FROM audit_log WHERE action = 'user.rename'")
        .fetch_one(&pool)
        .await
        .unwrap();
    assert_eq!(audits, 1);
    pool.close().await;
    e2e::drop_db(&name).await;
}
