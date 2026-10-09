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

    // And nothing else: no password hashes or emails.
    let err = sqlx::query("SELECT email FROM users LIMIT 1").execute(&pool).await.unwrap_err();
    assert!(err.to_string().contains("permission denied"), "{err}");

    pool.close().await;
    stack.shutdown().await;
}
