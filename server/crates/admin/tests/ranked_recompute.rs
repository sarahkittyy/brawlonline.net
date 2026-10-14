//! `admin ranked recompute` replays the rating history into every rank and placement, and writes
//! an audit row; `admin migrate` fills ranks missing after the migration. Runs the real `admin`
//! executable against a fresh database.

use accounts::store;
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

/// A rated set between `a` (winner) and `b`, written as accounts' settle would without ranks.
async fn rated_set(pool: &sqlx::PgPool, n: i32, a: Uuid, b: Uuid, ra: f64, rb: f64) -> (f64, f64) {
    let id = format!("mode.ranked-recompute-{n}");
    common::db::insert_match(pool, &id, 0, &[a, b], a, &[], "other", None).await.unwrap();
    let (na, nb) = (common::ranked::updated(ra, n as u32, rb, 1.0), common::ranked::updated(rb, n as u32, ra, 0.0));
    for (me, opp, before, after, opp_r, score) in [(a, b, ra, na, rb, 1.0), (b, a, rb, nb, ra, 0.0)] {
        sqlx::query(
            "INSERT INTO rating_events (uid, match_id, opponent, score, rating_before, rating_after,
                                        opponent_rating, k, sets_before, created_at)
             VALUES ($1, $2, $3, $4, $5, $6, $7, 32, $8, now() + make_interval(secs => $8))",
        )
        .bind(me)
        .bind(&id)
        .bind(opp)
        .bind(score)
        .bind(before)
        .bind(after)
        .bind(opp_r)
        .bind(n)
        .execute(pool)
        .await
        .unwrap();
    }
    (na, nb)
}

#[tokio::test]
async fn recompute_and_migrate_fill_ranks() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let a = store::create_user(&pool, "zz-rank-a@example.test", "x", "ann").await.unwrap().uid;
    let b = store::create_user(&pool, "zz-rank-b@example.test", "x", "bo").await.unwrap().uid;
    let (mut ra, mut rb) = (common::ranked::DEFAULT_RATING, common::ranked::DEFAULT_RATING);
    for n in 0..10 {
        (ra, rb) = rated_set(&pool, n, a, b, ra, rb).await;
    }
    for (uid, r) in [(a, ra), (b, rb)] {
        sqlx::query("INSERT INTO ratings (uid, rating, sets_played) VALUES ($1, $2, 10)")
            .bind(uid)
            .bind(r)
            .execute(&pool)
            .await
            .unwrap();
    }

    // Migrating again fills what is missing (the rows above have no rank or RD).
    let (ok, text) = run(&url, &["migrate"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("ranked tiers filled: 2 players from 20 rating changes"), "{text}");
    let (rank, rd): (Option<i16>, Option<f64>) =
        sqlx::query_as("SELECT rank, rd FROM ratings WHERE uid = $1").bind(a).fetch_one(&pool).await.unwrap();
    assert!(rd.is_some());
    let held = common::ranked::Rank::from_index(rank.expect("placed after ten sets")).unwrap();
    assert!(held >= common::ranked::Rank::for_rating(ra));

    // A wrong stored rank is put right, and audit-logged.
    sqlx::query("UPDATE ratings SET rank = 0 WHERE uid = $1").bind(a).execute(&pool).await.unwrap();
    let (ok, text) = run(&url, &["ranked", "recompute"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("2 players from 20 rating changes, 1 rank(s) changed"), "{text}");
    let back: Option<i16> =
        sqlx::query_scalar("SELECT rank FROM ratings WHERE uid = $1").bind(a).fetch_one(&pool).await.unwrap();
    assert_eq!(back, rank);
    let audits: i64 = sqlx::query_scalar("SELECT count(*) FROM audit_log WHERE action = 'ranked.recompute_tiers'")
        .fetch_one(&pool)
        .await
        .unwrap();
    assert_eq!(audits, 1);
    pool.close().await;
    e2e::drop_db(&name).await;
}
