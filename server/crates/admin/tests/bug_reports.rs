//! `admin bug list/show/resolve/reopen/delete/clear` against a fresh database, with the zips in a
//! temp folder (BUG_REPORTS_DIR). Runs the real `admin` executable.

use accounts::store;

async fn run(url: &str, dir: &std::path::Path, args: &[&str]) -> (bool, String) {
    let out = tokio::process::Command::new(env!("CARGO_BIN_EXE_admin"))
        .args(args)
        .current_dir(std::env::temp_dir())
        .env("DATABASE_URL", url)
        .env("BUG_REPORTS_DIR", dir)
        .output()
        .await
        .unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    (out.status.success(), text)
}

#[tokio::test]
async fn list_show_resolve_and_delete() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let dir = std::env::temp_dir().join(format!("pp-admin-bug-reports-{name}"));
    std::fs::create_dir_all(&dir).unwrap();
    let alice = store::create_user(&pool, "zz-bug-a@example.test", "x", "alice").await.unwrap();

    let (_, text) = run(&url, &dir, &["bug", "list"]).await;
    assert!(text.contains("no unresolved bug reports"), "{text}");

    let mut ids = vec![];
    for (uid, description, logs) in
        [(Some(alice.uid), "Desync in ranked\nSecond line", Some(2 * 1024 * 1024)), (None, "Can't log in", None)]
    {
        let id: i64 = sqlx::query_scalar(
            "INSERT INTO bug_reports (uid, description, launcher_version, platform, logs_bytes)
             VALUES ($1, $2, '1.4.2', 'linux x64', $3) RETURNING id",
        )
        .bind(uid)
        .bind(description)
        .bind(logs)
        .fetch_one(&pool)
        .await
        .unwrap();
        if logs.is_some() {
            std::fs::write(dir.join(format!("bug-{id}.zip")), b"PK\x05\x06").unwrap();
        }
        ids.push(id);
    }
    let (a, b) = (ids[0].to_string(), ids[1].to_string());

    let (ok, text) = run(&url, &dir, &["bug", "list"]).await;
    assert!(ok, "{text}");
    let (first, second) = (text.find("Can't log in").unwrap(), text.find("Desync in ranked").unwrap());
    assert!(first < second, "newest first: {text}");
    for want in ["(alice)", "[2.0 MB]", "[no logs]", "(logged out or deleted account)", "Desync in ranked ..."] {
        assert!(text.contains(want), "{want:?} missing: {text}");
    }
    assert!(!text.contains("Second line"), "list shows the first line only: {text}");

    let (ok, text) = run(&url, &dir, &["bug", "show", &a]).await;
    assert!(ok, "{text}");
    for want in ["launcher:   1.4.2", "platform:   linux x64", &format!("bug-{a}.zip"), "Second line"] {
        assert!(text.contains(want), "{want:?} missing: {text}");
    }

    let (ok, text) = run(&url, &dir, &["bug", "resolve", &a, "--note", "fixed in 1.4.3"]).await;
    assert!(ok && text.contains("resolved"), "{text}");
    let (ok, _) = run(&url, &dir, &["bug", "resolve", &a]).await;
    assert!(!ok, "twice: an error");
    let (_, text) = run(&url, &dir, &["bug", "list"]).await;
    assert!(!text.contains("Desync") && text.contains("Can't log in"), "{text}");
    let (_, text) = run(&url, &dir, &["bug", "list", "--all"]).await;
    assert!(text.contains("Desync") && text.contains("RESOLVED"), "{text}");
    let (_, text) = run(&url, &dir, &["bug", "show", &a]).await;
    assert!(text.contains("resolution: fixed in 1.4.3"), "{text}");

    let (ok, _) = run(&url, &dir, &["bug", "reopen", &a]).await;
    assert!(ok);
    let (_, text) = run(&url, &dir, &["bug", "list"]).await;
    assert!(text.contains("Desync"), "{text}");
    run(&url, &dir, &["bug", "resolve", &a]).await;

    // clear: only resolved ones, with their zips; --yes required.
    let (ok, _) = run(&url, &dir, &["bug", "clear"]).await;
    assert!(!ok);
    assert!(dir.join(format!("bug-{a}.zip")).exists());
    let (ok, text) = run(&url, &dir, &["bug", "clear", "--yes"]).await;
    assert!(ok && text.contains("deleted 1 resolved"), "{text}");
    assert!(!dir.join(format!("bug-{a}.zip")).exists());
    let (_, text) = run(&url, &dir, &["bug", "list", "--all"]).await;
    assert!(!text.contains("Desync") && text.contains("Can't log in"), "{text}");

    // delete: by id; an unknown id is an error.
    let (ok, text) = run(&url, &dir, &["bug", "delete", &b, "999999"]).await;
    assert!(!ok && text.contains("deleted 1") && text.contains("999999"), "{text}");
    let (_, text) = run(&url, &dir, &["bug", "list", "--all"]).await;
    assert!(text.contains("no bug reports"), "{text}");

    let actions: Vec<String> =
        sqlx::query_scalar("SELECT action FROM audit_log WHERE action LIKE 'bug_report.%' ORDER BY id")
            .fetch_all(&pool)
            .await
            .unwrap();
    assert_eq!(
        actions,
        ["bug_report.resolve", "bug_report.reopen", "bug_report.resolve", "bug_report.delete", "bug_report.delete"]
    );
    pool.close().await;
    e2e::drop_db(&name).await;
    let _ = std::fs::remove_dir_all(&dir);
}
