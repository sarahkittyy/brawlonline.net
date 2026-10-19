//! `admin ruleset delete CODE` and `admin ruleset rename CODE NAME` (the user's, 2026-10-19): an
//! operator deletes or renames a published version (or with `--all-versions` the whole ruleset),
//! which the database refuses everyone else (0014_ruleset_plays.sql). Runs the real `admin`
//! executable against a fresh database.

use accounts::store;
use common::rules::Ruleset;
use sqlx::PgPool;
use uuid::Uuid;

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

/// Publishes version `version` of `lineage` as accounts does (canonical JSON, its hash, a 0 count).
async fn publish(pool: &PgPool, owner: Uuid, lineage: Uuid, code: &str, version: u32, name: &str) -> Ruleset {
    let mut r = Ruleset { name: name.into(), version, best_of: 3, strikes: vec![1, 2, 1], ..Default::default() };
    r.normalize();
    sqlx::query(
        "INSERT INTO rulesets (code, owner, lineage, version, name, publisher_name, json, hash)
         VALUES ($1, $2, $3, $4, $5, 'alice', $6, $7)",
    )
    .bind(code)
    .bind(owner)
    .bind(lineage)
    .bind(version as i32)
    .bind(name)
    .bind(r.canonical_json())
    .bind(r.hash())
    .execute(pool)
    .await
    .unwrap();
    sqlx::query("INSERT INTO ruleset_plays (code, games) VALUES ($1, 5)").bind(code).execute(pool).await.unwrap();
    r
}

#[tokio::test]
async fn delete_and_rename_rulesets() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-rulesets@example.test", "x", "alice").await.unwrap();
    let lineage = Uuid::new_v4();
    publish(&pool, alice.uid, lineage, "BCDFGH", 1, "Weekly").await;
    let v2 = publish(&pool, alice.uid, lineage, "CDFGHJ", 2, "Weekly").await;
    let other = publish(&pool, alice.uid, Uuid::new_v4(), "DFGHJK", 1, "Other").await;
    let row = |code: &'static str| {
        let pool = pool.clone();
        async move {
            sqlx::query_as::<_, (String, String, String)>("SELECT name, json, hash FROM rulesets WHERE code = $1")
                .bind(code)
                .fetch_optional(&pool)
                .await
                .unwrap()
        }
    };

    // Nobody else may: the database still refuses the change and the delete.
    let err = sqlx::query("UPDATE rulesets SET name = 'x' WHERE code = 'BCDFGH'").execute(&pool).await.unwrap_err();
    assert!(err.to_string().contains("never change"), "{err}");
    let err = sqlx::query("DELETE FROM rulesets WHERE code = 'BCDFGH'").execute(&pool).await.unwrap_err();
    assert!(err.to_string().contains("never deleted"), "{err}");

    // Rename one version: its name, JSON and hash; the other version is left alone.
    let (ok, text) = run(&url, &["ruleset", "rename", "bcdfgh", "Weekly Bo3"]).await;
    assert!(ok, "{text}");
    assert!(text.contains(r#"BCDFGH v1 renamed from "Weekly" to "Weekly Bo3""#), "{text}");
    assert!(text.contains("left alone: CDFGHJ (v2)"), "{text}");
    let (n, json, hash) = row("BCDFGH").await.unwrap();
    let r: Ruleset = serde_json::from_str(&json).unwrap();
    assert_eq!((n.as_str(), r.name.as_str(), r.version, hash), ("Weekly Bo3", "Weekly Bo3", 1, r.hash()));
    assert_eq!(row("CDFGHJ").await.unwrap().0, "Weekly");
    // Every version; a name the publisher couldn't use changes nothing.
    let (ok, text) = run(&url, &["ruleset", "rename", "CDFGHJ", "Tourney", "--all-versions"]).await;
    assert!(ok, "{text}");
    assert_eq!((row("BCDFGH").await.unwrap().0, row("CDFGHJ").await.unwrap().0), ("Tourney".into(), "Tourney".into()));
    for bad in ["", " padded ", "a name that is far too long for any ruleset at all, really"] {
        let (ok, text) = run(&url, &["ruleset", "rename", "CDFGHJ", bad]).await;
        assert!(!ok, "{bad:?} accepted: {text}");
    }
    assert_eq!(row("CDFGHJ").await.unwrap().0, "Tourney");
    let (ok, text) = run(&url, &["ruleset", "rename", "ZZZZZZ", "x"]).await;
    assert!(!ok && text.contains("no ruleset ZZZZZZ"), "{text}");

    // Delete v2: it is remembered, recent and a draft's base. Without --yes nothing happens.
    sqlx::query("INSERT INTO remembered_rulesets (uid, code) VALUES ($1, 'CDFGHJ')")
        .bind(alice.uid)
        .execute(&pool)
        .await
        .unwrap();
    sqlx::query("INSERT INTO ruleset_recent (uid, code) VALUES ($1, 'CDFGHJ'), ($1, 'DFGHJK')")
        .bind(alice.uid)
        .execute(&pool)
        .await
        .unwrap();
    let draft = Uuid::new_v4();
    sqlx::query("INSERT INTO ruleset_drafts (id, owner, based_on, name, json) VALUES ($1, $2, 'CDFGHJ', 'd', $3)")
        .bind(draft)
        .bind(alice.uid)
        .bind(sqlx::types::Json(&v2))
        .execute(&pool)
        .await
        .unwrap();
    let (ok, text) = run(&url, &["ruleset", "delete", "CDFGHJ"]).await;
    assert!(!ok && text.contains("--yes"), "{text}");
    assert!(row("CDFGHJ").await.is_some());
    let (ok, text) = run(&url, &["ruleset", "delete", "CDFGHJ", "--yes"]).await;
    assert!(ok, "{text}");
    assert!(text.contains(r#"deleted CDFGHJ ("Tourney" v2)"#) && text.contains("left alone: BCDFGH (v1)"), "{text}");
    assert!(row("CDFGHJ").await.is_none());
    let count = |sql: &'static str| {
        let pool = pool.clone();
        async move { sqlx::query_scalar::<_, i64>(sql).fetch_one(&pool).await.unwrap() }
    };
    assert_eq!(count("SELECT count(*) FROM remembered_rulesets").await, 0);
    assert_eq!(count("SELECT count(*) FROM ruleset_recent").await, 1);
    assert_eq!(count("SELECT count(*) FROM ruleset_plays WHERE code = 'CDFGHJ'").await, 0);
    assert_eq!(count("SELECT count(*) FROM ruleset_drafts WHERE based_on IS NULL").await, 1);
    // mm counting a game for the deleted version counts nothing and fails nothing.
    common::db::count_ruleset_game(&pool, "CDFGHJ").await.unwrap();
    // The whole ruleset; the other ruleset stays.
    let (ok, text) = run(&url, &["ruleset", "delete", "BCDFGH", "--all-versions", "--yes"]).await;
    assert!(ok, "{text}");
    assert!(row("BCDFGH").await.is_none());
    assert_eq!(row("DFGHJK").await.unwrap().2, other.hash());
    assert_eq!(count("SELECT count(*) FROM audit_log WHERE action = 'ruleset.delete'").await, 2);
    assert_eq!(count("SELECT count(*) FROM audit_log WHERE action = 'ruleset.rename'").await, 2);
    pool.close().await;
    e2e::drop_db(&name).await;
}
