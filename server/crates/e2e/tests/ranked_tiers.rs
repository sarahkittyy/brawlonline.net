//! End-to-end: ranked tiers (docs/design/ranked-tiers.md). Sets played through the report endpoint
//! place both players and give them ranks; `/user/{uid}` and the leaderboard show `tier` / `rank`
//! and `setsToPlace`; the settle's ranks and RDs are exactly what replaying the rating history gives
//! (so `recompute_tiers` changes nothing); the backfill after the migration fills a database whose
//! ranks are missing, once.

use chrono::{Duration, Utc};
use common::ranked::{self, Rank, RD_START};
use e2e::{Stack, StackOptions, UserJson};
use serde_json::{json, Value};
use uuid::Uuid;

fn uuid(u: &UserJson) -> Uuid {
    Uuid::parse_str(&u.uid).unwrap()
}

/// One ranked set: both players report the same two games, `winner` takes both.
async fn play_set(stack: &Stack, a: &UserJson, b: &UserJson, winner: &UserJson, n: i64) -> String {
    let id = format!("mode.ranked-tiers-{n}-{}", Uuid::new_v4().simple());
    let p = [uuid(a), uuid(b)];
    common::db::insert_match(&stack.pool, &id, 0, &p, p[0], &[], "other", None).await.unwrap();
    sqlx::query("UPDATE mm_matches SET created_at = $2 WHERE match_id = $1")
        .bind(&id)
        .bind(Utc::now() - Duration::minutes(30) + Duration::seconds(n))
        .execute(&stack.pool)
        .await
        .unwrap();
    for game in 1..=2u32 {
        for by in [a, b] {
            let (st, body) = stack
                .post(
                    "/v1/ranked/report-game",
                    None,
                    json!({
                        "uid": by.uid, "playKey": by.play_key, "matchId": id, "gameIndex": game,
                        "winner": winner.uid, "stageId": 31, "durationFrames": 9000,
                        "players": [{"uid": by.uid, "characterId": 9, "stocksRemaining": 2, "damage": 40.5}],
                    }),
                )
                .await;
            assert_eq!(st, 200, "{body}");
        }
    }
    let status: String = sqlx::query_scalar("SELECT status FROM mm_matches WHERE match_id = $1")
        .bind(&id)
        .fetch_one(&stack.pool)
        .await
        .unwrap();
    assert_eq!(status, "COMPLETE");
    id
}

async fn user_rank(stack: &Stack, u: &UserJson) -> Value {
    let (st, body) = stack.get(&format!("/user/{}", u.uid), None).await;
    assert_eq!(st, 200, "{body}");
    body["rank"].clone()
}

async fn board_entry(stack: &Stack, u: &UserJson) -> Value {
    let (st, body) = stack.get("/v1/ranked/leaderboard?limit=100", None).await;
    assert_eq!(st, 200, "{body}");
    body["entries"].as_array().unwrap().iter().find(|e| e["uid"] == u.uid.as_str()).cloned().unwrap()
}

fn view(rank: Option<Rank>) -> Value {
    match rank {
        None => Value::Null,
        Some(r) => json!({ "tier": r.tier().as_str(), "division": r.division() }),
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sets_place_players_and_the_settle_matches_the_replay() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, a) = stack.create_player("tiers-a@example.test", "ana", "AN").await;
    let (_, b) = stack.create_player("tiers-b@example.test", "ben", "BE").await;

    // Before any set: no rank, five sets to place, and not on the leaderboard yet.
    let r = user_rank(&stack, &a).await;
    assert_eq!(r["tier"], Value::Null);
    assert_eq!(r["setsToPlace"], ranked::PLACEMENT_SETS);

    // Ana wins most sets. Two new players tell each other little (a high RD), but placement only
    // counts sets: "N more" drops by one every set and the rank comes with the fifth.
    for n in 0..12i64 {
        let winner = if n % 4 == 3 { &b } else { &a };
        play_set(&stack, &a, &b, winner, n).await;
        let r = user_rank(&stack, &a).await;
        let left = r["setsToPlace"].as_u64().unwrap();
        let played = n as u32 + 1;
        assert_eq!(left, u64::from(ranked::PLACEMENT_SETS.saturating_sub(played)), "set {n}: {r}");
        assert_eq!(r["tier"] == Value::Null, left > 0, "set {n}: {r}");
        // The leaderboard says the same.
        let e = board_entry(&stack, &a).await;
        assert_eq!(e["rank"], r["tier"], "set {n}");
        assert_eq!(e["setsToPlace"], r["setsToPlace"], "set {n}");
    }

    // Stored: every rating change has its ranks and RD; ratings has the player's.
    let (rank, rd): (Option<i16>, Option<f64>) = sqlx::query_as("SELECT rank, rd FROM ratings WHERE uid = $1")
        .bind(uuid(&a))
        .fetch_one(&stack.pool)
        .await
        .unwrap();
    assert!(rd.unwrap() < RD_START);
    let rank = Rank::from_index(rank.unwrap()).unwrap();
    assert_eq!(view(Some(rank)), user_rank(&stack, &a).await["tier"]);
    let missing: i64 = sqlx::query_scalar("SELECT count(*) FROM rating_events WHERE rd_after IS NULL")
        .fetch_one(&stack.pool)
        .await
        .unwrap();
    assert_eq!(missing, 0);

    // Replaying the history gives exactly what the settles stored.
    #[allow(clippy::type_complexity)]
    let before: Vec<(i64, Option<i16>, Option<i16>, Option<f64>)> =
        sqlx::query_as("SELECT id, rank_before, rank_after, rd_after FROM rating_events ORDER BY id")
            .fetch_all(&stack.pool)
            .await
            .unwrap();
    let done = accounts::ranked::recompute_tiers(&stack.pool).await.unwrap();
    assert_eq!(done.sets, 24);
    assert_eq!(done.players, 2);
    assert_eq!(done.ranks_changed, 0);
    #[allow(clippy::type_complexity)]
    let after: Vec<(i64, Option<i16>, Option<i16>, Option<f64>)> =
        sqlx::query_as("SELECT id, rank_before, rank_after, rd_after FROM rating_events ORDER BY id")
            .fetch_all(&stack.pool)
            .await
            .unwrap();
    assert_eq!(before.len(), after.len());
    for (x, y) in before.iter().zip(&after) {
        assert_eq!((x.0, x.1, x.2), (y.0, y.1, y.2));
        assert!((x.3.unwrap() - y.3.unwrap()).abs() < 1e-9);
    }
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_backfill_fills_missing_ranks_once() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, a) = stack.create_player("fill-a@example.test", "cam", "CA").await;
    let (_, b) = stack.create_player("fill-b@example.test", "dee", "DE").await;
    for n in 0..10i64 {
        play_set(&stack, &a, &b, &a, n).await;
    }
    let placed = user_rank(&stack, &a).await;
    assert_ne!(placed["tier"], Value::Null);

    // As right after the migration: nothing filled.
    sqlx::query("UPDATE ratings SET rank = NULL, rd = NULL").execute(&stack.pool).await.unwrap();
    sqlx::query("UPDATE rating_events SET rank_before = NULL, rank_after = NULL, rd_after = NULL")
        .execute(&stack.pool)
        .await
        .unwrap();
    let r = user_rank(&stack, &a).await;
    assert_eq!(r["tier"], Value::Null);
    let filled = accounts::ranked::backfill_tiers(&stack.pool).await.unwrap().expect("filled");
    assert_eq!((filled.sets, filled.players), (20, 2));
    assert_eq!(user_rank(&stack, &a).await, placed);
    assert!(accounts::ranked::backfill_tiers(&stack.pool).await.unwrap().is_none(), "only once");

    // A stored placement from an older rule (RD-based until 2026-10-16) is recomputed too.
    sqlx::query("UPDATE ratings SET rank = NULL").execute(&stack.pool).await.unwrap();
    assert!(accounts::ranked::backfill_tiers(&stack.pool).await.unwrap().is_some(), "recomputed");
    assert_eq!(user_rank(&stack, &a).await, placed);
    assert!(accounts::ranked::backfill_tiers(&stack.pool).await.unwrap().is_none(), "only once");
    stack.shutdown().await;
}
