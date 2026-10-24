//! `admin ranked void` takes a rated set back and re-rates every set settled since, as if it had
//! never been played. Runs the real `admin` executable against a fresh database.

use std::collections::HashMap;

use accounts::store;
use common::ranked::Standing;
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

/// One set: its id, its status, and its rated players as (uid, opponent, score).
type Set = (&'static str, &'static str, Vec<(Uuid, Uuid, f64)>);

/// Rates `sets` in order as accounts' settle does, into `st`; with `pool`, stores them too (a set
/// a second apart, `ratings` rows at the end). Returns each rated player's standing.
async fn play(pool: Option<&PgPool>, sets: &[Set]) -> HashMap<Uuid, Standing> {
    let mut st: HashMap<Uuid, Standing> = HashMap::new();
    let mut wl: HashMap<Uuid, (i32, i32)> = HashMap::new();
    for (n, (id, status, rated)) in sets.iter().enumerate() {
        let before = st.clone();
        if let Some(pool) = pool {
            let players: Vec<Uuid> = rated.iter().flat_map(|r| [r.0, r.1]).take(2).collect();
            common::db::insert_match(pool, id, 0, &players, players[0], &[], "other", None).await.unwrap();
            sqlx::query("UPDATE mm_matches SET status = $2, end_reason = 'x' WHERE match_id = $1")
                .bind(id)
                .bind(status)
                .execute(pool)
                .await
                .unwrap();
        }
        for &(me, opp, score) in rated {
            let (s, o) = (before.get(&me).copied().unwrap_or_default(), before.get(&opp).copied().unwrap_or_default());
            let after = common::ranked::updated(s.rating, s.sets_played, o.rating, score);
            if let Some(pool) = pool {
                sqlx::query(
                    "INSERT INTO rating_events (uid, match_id, opponent, score, rating_before, rating_after,
                                                opponent_rating, k, sets_before, created_at)
                     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, now() + make_interval(secs => $10))",
                )
                .bind(me)
                .bind(id)
                .bind(opp)
                .bind(score)
                .bind(s.rating)
                .bind(after)
                .bind(o.rating)
                .bind(common::ranked::k_factor(s.sets_played))
                .bind(s.sets_played as i32)
                .bind(n as f64)
                .execute(pool)
                .await
                .unwrap();
            }
            st.insert(me, Standing { rating: after, sets_played: s.sets_played + 1 });
            let e = wl.entry(me).or_default();
            if score > 0.5 {
                e.0 += 1;
            } else {
                e.1 += 1;
            }
        }
    }
    if let Some(pool) = pool {
        for (uid, s) in &st {
            sqlx::query("INSERT INTO ratings (uid, rating, sets_played, wins, losses) VALUES ($1, $2, $3, $4, $5)")
                .bind(uid)
                .bind(s.rating)
                .bind(s.sets_played as i32)
                .bind(wl[uid].0)
                .bind(wl[uid].1)
                .execute(pool)
                .await
                .unwrap();
        }
    }
    st
}

fn won(id: &'static str, w: Uuid, l: Uuid) -> Set {
    (id, "COMPLETE", vec![(w, l, 1.0), (l, w, 0.0)])
}

async fn rating_row(pool: &PgPool, uid: Uuid) -> (f64, i32, i32, i32, Option<i16>) {
    sqlx::query_as("SELECT rating, sets_played, wins, losses, rank FROM ratings WHERE uid = $1")
        .bind(uid)
        .fetch_one(pool)
        .await
        .unwrap()
}

#[tokio::test]
async fn voiding_a_set_rerates_every_later_set() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let mut u = vec![];
    for (i, n) in ["ann", "bo", "cy", "di"].iter().enumerate() {
        u.push(store::create_user(&pool, &format!("zz-void-{i}@example.test"), "x", n).await.unwrap().uid);
    }
    let (a, b, c, d) = (u[0], u[1], u[2], u[3]);
    // Ann abandons the set to void before any game (only she is rated, as such a set was until
    // 2026-10-23); her later sets, and through them Bo's, Cy's and Di's, were rated from the lower
    // rating.
    let mut sets = vec![won("v-1", a, b), won("v-2", c, d), won("v-3", b, c), won("v-4", a, d), won("v-5", d, b)];
    sets.push(("v-void", "ABANDONED", vec![(a, b, 0.0)]));
    sets.extend([won("v-6", a, c), won("v-7", c, b), won("v-8", b, a), won("v-9", d, c), won("v-10", a, d)]);
    play(Some(&pool), &sets).await;
    let (ok, text) = run(&url, &["ranked", "recompute"]).await;
    assert!(ok, "{text}");
    let before: Vec<_> = [a, b, c, d].iter().map(|&p| rating_row(&pool, p)).collect();
    let before = futures_join(before).await;

    // A dry run changes nothing.
    let (ok, text) = run(&url, &["ranked", "void", "v-void", "--reason", "client hang", "--dry-run"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("DRY RUN"), "{text}");
    assert!(text.contains("1 rating change(s) deleted, 10 of 10 later ones re-rated, 4 player(s) moved"), "{text}");
    assert_eq!(futures_join([a, b, c, d].iter().map(|&p| rating_row(&pool, p)).collect()).await, before);
    let audits: i64 = sqlx::query_scalar("SELECT count(*) FROM audit_log WHERE action = 'ranked.void_set'")
        .fetch_one(&pool)
        .await
        .unwrap();
    assert_eq!(audits, 0);

    let (ok, text) = run(&url, &["ranked", "void", "v-void", "--reason", "client hang"]).await;
    assert!(ok, "{text}");
    assert!(!text.contains("DRY RUN"), "{text}");

    // Everyone stands where the history without the set leaves them.
    let without: Vec<Set> = sets.iter().filter(|s| s.0 != "v-void").cloned().collect();
    let expected = play(None, &without).await;
    for p in [a, b, c, d] {
        let (rating, n, ..) = rating_row(&pool, p).await;
        assert_eq!((rating, n as u32), (expected[&p].rating, expected[&p].sets_played), "{p}");
    }
    // Ann gets the set and the loss back; ranks are filled.
    let (_, n, w, l, rank) = rating_row(&pool, a).await;
    assert_eq!((n, w + l), (before[0].1 - 1, before[0].2 + before[0].3 - 1));
    assert_eq!(l, before[0].3 - 1);
    assert!(rank.is_some());
    // The stored history is continuous again: every row starts where the player's last one ended.
    let breaks: i64 = sqlx::query_scalar(
        "SELECT count(*) FROM (SELECT rating_before, sets_before,
                lag(rating_after) OVER w AS prev, lag(sets_before) OVER w AS prev_n
           FROM rating_events WINDOW w AS (PARTITION BY uid ORDER BY created_at, id)) x
          WHERE prev IS NOT NULL AND (prev <> rating_before OR sets_before <> prev_n + 1)",
    )
    .fetch_one(&pool)
    .await
    .unwrap();
    assert_eq!(breaks, 0);
    let (status, winner, reason): (String, Option<Uuid>, String) =
        sqlx::query_as("SELECT status, winner, end_reason FROM mm_matches WHERE match_id = 'v-void'")
            .fetch_one(&pool)
            .await
            .unwrap();
    assert_eq!((status.as_str(), winner, reason.as_str()), ("TERMINATED", None, "voided by admin: client hang"));
    let detail: serde_json::Value =
        sqlx::query_scalar("SELECT detail FROM audit_log WHERE action = 'ranked.void_set' AND target = 'v-void'")
            .fetch_one(&pool)
            .await
            .unwrap();
    assert_eq!(detail["previous_status"], "ABANDONED");
    assert_eq!(detail["events"].as_array().unwrap().len(), 1);
    assert_eq!(detail["rerated"].as_array().unwrap().len(), 10);

    // A voided set cannot be voided again.
    let (ok, text) = run(&url, &["ranked", "void", "v-void", "--reason", "again"]).await;
    assert!(!ok && text.contains("is TERMINATED"), "{text}");
    pool.close().await;
    e2e::drop_db(&name).await;
}

#[tokio::test]
async fn a_history_that_does_not_replay_is_refused() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let a = store::create_user(&pool, "zz-void-x@example.test", "x", "ann").await.unwrap().uid;
    let b = store::create_user(&pool, "zz-void-y@example.test", "x", "bo").await.unwrap().uid;
    play(Some(&pool), &[won("w-1", a, b), won("w-2", b, a), won("w-3", a, b)]).await;
    // A row corrected by hand that nothing re-rated after it.
    sqlx::query("UPDATE rating_events SET rating_after = rating_after + 5 WHERE match_id = 'w-2' AND uid = $1")
        .bind(b)
        .execute(&pool)
        .await
        .unwrap();
    let (ok, text) = run(&url, &["ranked", "void", "w-1", "--reason", "test"]).await;
    assert!(!ok, "{text}");
    assert!(text.contains("does not reproduce") && text.contains("nothing was changed"), "{text}");
    let status: String =
        sqlx::query_scalar("SELECT status FROM mm_matches WHERE match_id = 'w-1'").fetch_one(&pool).await.unwrap();
    assert_eq!(status, "COMPLETE");
    // Not ranked, unknown, no reason.
    let (ok, text) = run(&url, &["ranked", "void", "nope", "--reason", "test"]).await;
    assert!(!ok && text.contains("no match"), "{text}");
    let (ok, text) = run(&url, &["ranked", "void", "w-3", "--reason", " "]).await;
    assert!(!ok && text.contains("a reason is required"), "{text}");
    pool.close().await;
    e2e::drop_db(&name).await;
}

async fn futures_join<F: std::future::Future>(fs: Vec<F>) -> Vec<F::Output> {
    let mut out = vec![];
    for f in fs {
        out.push(f.await);
    }
    out
}
