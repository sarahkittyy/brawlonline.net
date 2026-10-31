//! `admin user slur-log` lists every slur replacement in the audit log, newest first, with the
//! account as it is now; other audit rows are left out. Runs the real `admin` executable against a
//! fresh database.

use accounts::slur_filter::{audit_replacement, ACTOR};
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
async fn slur_log_lists_replacements() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();

    let (ok, text) = run(&url, &["user", "slur-log"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("no slur replacements"), "{text}");

    let u = store::create_user(&pool, "zz-slurlog@example.test", "x", "happy friend").await.unwrap();
    // As written before 2026-10-31.
    store::audit(
        &pool,
        ACTOR,
        "user.rename",
        Some(&u.uid.to_string()),
        serde_json::json!({"from": "xXoldwordXx", "to": "kind heart", "reason": "racial slur"}),
    )
    .await
    .unwrap();
    audit_replacement(&pool, ACTOR, "user.signup_name", u.uid, "xXbadwordXx", "happy friend").await.unwrap();
    audit_replacement(&pool, "admin-cli:sarah", "user.set_code", u.uid, "BADW#12", "JOY#34").await.unwrap();
    store::audit(
        &pool,
        "admin-cli:sarah",
        "user.rename",
        Some(&u.uid.to_string()),
        serde_json::json!({"from": "a", "to": "b"}),
    )
    .await
    .unwrap();

    let (ok, text) = run(&url, &["user", "slur-log"]).await;
    assert!(ok, "{text}");
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(lines.len(), 3, "{text}");
    assert!(
        lines[0].contains("admin-cli:sarah") && lines[0].contains(r#"code         "BADW#12" -> "JOY#34""#),
        "{text}"
    );
    assert!(
        lines[1].contains("slur-filter") && lines[1].contains(r#"sign-up name "xXbadwordXx" -> "happy friend""#),
        "{text}"
    );
    assert!(lines[1].contains(&format!(r#"now - "happy friend" ({})"#, u.uid)), "{text}");
    assert!(lines[2].contains(r#""xXoldwordXx" -> "kind heart""#), "{text}");
    let (ok, text) = run(&url, &["user", "slur-log", "--limit", "1"]).await;
    assert!(ok && text.lines().count() == 1, "{text}");
    pool.close().await;
    e2e::drop_db(&name).await;
}
