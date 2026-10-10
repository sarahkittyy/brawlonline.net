//! mm's queries work as production's `pp_mm` role, with only the privileges the migrations grant
//! it (0005_mm_grants.sql). A migration that adds something mm reads without granting it fails
//! here instead of in production ("Matchmaking unavailable", 2026-10-09).

use common::db;
use common::ranked::DEFAULT_RATING;
use e2e::{Stack, StackOptions};
use sqlx::postgres::PgPoolOptions;
use sqlx::{Executor, PgPool};
use uuid::Uuid;

/// Creates the cluster-wide role `pp_mm` (as production has it) if it is missing. Must run before
/// the database is migrated, since the grants only happen where the role exists.
async fn ensure_mm_role() {
    let admin = PgPool::connect(&e2e::ensure_postgres().await.unwrap()).await.unwrap();
    // Tests run in parallel: a concurrent CREATE ROLE can fail with unique_violation.
    admin
        .execute(
            "DO $$ BEGIN CREATE ROLE pp_mm NOLOGIN;
             EXCEPTION WHEN duplicate_object OR unique_violation THEN NULL; END $$",
        )
        .await
        .unwrap();
    admin.close().await;
}

/// A pool on the stack's database whose connections act as `pp_mm`.
async fn mm_pool(stack: &Stack) -> PgPool {
    PgPoolOptions::new()
        .max_connections(1)
        .after_connect(|conn, _| {
            Box::pin(async move {
                conn.execute("SET ROLE pp_mm").await?;
                Ok(())
            })
        })
        .connect(&e2e::db_url(&stack.db_name).await.unwrap())
        .await
        .unwrap()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn mm_queries_work_as_pp_mm() {
    ensure_mm_role().await;
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let a: Uuid = alice.uid.parse().unwrap();
    let b: Uuid = bob.uid.parse().unwrap();
    // Bob has a ratings row, Alice none.
    sqlx::query("INSERT INTO ratings (uid, rating, sets_played) VALUES ($1, 1234.5, 3)")
        .bind(b)
        .execute(&stack.pool)
        .await
        .unwrap();

    let pool = mm_pool(&stack).await;
    let role: String = sqlx::query_scalar("SELECT current_user::text").fetch_one(&pool).await.unwrap();
    assert_eq!(role, "pp_mm");

    let ua = db::fetch_mm_user(&pool, a).await.unwrap().unwrap();
    assert_eq!(ua.connect_code.as_deref(), Some(alice.connect_code.as_str()));
    assert_eq!(ua.rating, DEFAULT_RATING);
    let ub = db::fetch_mm_user(&pool, b).await.unwrap().unwrap();
    assert_eq!(ub.rating, 1234.5);
    assert_eq!(ub.ranked_sets, 3);

    let id = format!("mode.ranked-grants-{}", Uuid::new_v4());
    db::insert_match(&pool, &id, 0, &[a, b], a, &[1, 2], "test").await.unwrap();
    // A second insert of the same match is a no-op (ON CONFLICT needs SELECT (match_id)).
    db::insert_match(&pool, &id, 0, &[a, b], a, &[1, 2], "test").await.unwrap();

    // Chat (0006_chat.sql): identity keys recorded (again, and pruned past 20), read, a report
    // stored, also one about a uid that is no account.
    for i in 0..22u8 {
        db::record_chat_key(&pool, a, &[i; 32]).await.unwrap();
    }
    db::record_chat_key(&pool, a, &[21; 32]).await.unwrap();
    let keys = db::fetch_chat_keys(&pool, a).await.unwrap();
    assert_eq!(keys.len(), db::CHAT_KEYS_KEPT as usize);
    assert_eq!(keys[0], [21; 32]);
    assert!(!keys.contains(&[0; 32]) && !keys.contains(&[1; 32]));
    let report = db::NewChatReport {
        reporter: a,
        reported: b,
        group_id: "room-KFQB-1".into(),
        reason: "spam".into(),
        messages: serde_json::json!([{"seq": 1, "text": "hi", "raw": "6869", "sig": "00", "verified": false}]),
        verified: 0,
        total: 1,
    };
    db::insert_chat_report(&pool, &report).await.unwrap();
    db::insert_chat_report(&pool, &db::NewChatReport { reported: Uuid::new_v4(), ..report }).await.unwrap();
    let rows: Vec<(Option<Uuid>, Option<Uuid>)> =
        sqlx::query_as("SELECT reporter, reported FROM chat_reports ORDER BY id").fetch_all(&stack.pool).await.unwrap();
    assert_eq!(rows, vec![(Some(a), Some(b)), (Some(a), None)]);
    // Reports are write-only for mm.
    let err = sqlx::query("SELECT reason FROM chat_reports").execute(&pool).await.unwrap_err();
    assert!(err.to_string().contains("permission denied"), "{err}");

    // And nothing else: no password hashes or emails.
    let err = sqlx::query("SELECT email FROM users LIMIT 1").execute(&pool).await.unwrap_err();
    assert!(err.to_string().contains("permission denied"), "{err}");

    pool.close().await;
    stack.shutdown().await;
}
