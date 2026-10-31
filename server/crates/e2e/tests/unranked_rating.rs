//! Unranked's hidden rating end to end: the players' game reports move it once per game, mm's
//! account query reads it, and the rebuild (accounts' first start, `pp-admin unranked rebuild`)
//! comes to the same numbers.

use common::db;
use common::unranked::{self, Standing};
use e2e::{Stack, StackOptions, UserJson};
use serde_json::{json, Value};
use uuid::Uuid;

fn uuid(u: &UserJson) -> Uuid {
    u.uid.parse().unwrap()
}

async fn mm_match(stack: &Stack, mode: i16, a: &UserJson, b: &UserJson) -> String {
    let id = format!("mode.test-{mode}-{}", Uuid::new_v4().simple());
    let p = [uuid(a), uuid(b)];
    db::insert_match(&stack.pool, &id, mode, &p, p[0], &[], "other", None).await.unwrap();
    id
}

async fn report(stack: &Stack, by: &UserJson, id: &str, game: u32, winner: Option<&UserJson>) -> Value {
    let (st, body) = stack
        .post(
            "/v1/ranked/report-game",
            None,
            json!({
                "uid": by.uid, "playKey": by.play_key, "matchId": id, "gameIndex": game,
                "winner": winner.map(|w| w.uid.clone()), "stageId": 31, "durationFrames": 9000,
                "players": [{"uid": by.uid, "characterId": 9, "stocksRemaining": 2, "damage": 40.5}],
            }),
        )
        .await;
    assert_eq!(st, 200, "{body}");
    body
}

async fn stored(stack: &Stack, u: &UserJson) -> Option<(f64, i32)> {
    sqlx::query_as("SELECT rating, games FROM unranked_ratings WHERE uid = $1")
        .bind(uuid(u))
        .fetch_optional(&stack.pool)
        .await
        .unwrap()
}

/// The Unranked rating mm pairs `u` by.
async fn mm_view(stack: &Stack, u: &UserJson) -> f64 {
    db::fetch_mm_user(&stack.pool, uuid(u)).await.unwrap().unwrap().unranked_rating
}

async fn rated_games(stack: &Stack) -> i64 {
    sqlx::query_scalar("SELECT count(*) FROM unranked_rated_games").fetch_one(&stack.pool).await.unwrap()
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 1e-6
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn unranked_games_move_the_hidden_rating_once_each() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "al").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    // Alice has played Ranked (1700); Bob never has (1400).
    sqlx::query("INSERT INTO ratings (uid, rating, sets_played) VALUES ($1, 1700, 12)")
        .bind(uuid(&alice))
        .execute(&stack.pool)
        .await
        .unwrap();
    // Before any Unranked game, mm sees the Ranked rating.
    assert_eq!(mm_view(&stack, &alice).await, 1700.0);
    assert_eq!(mm_view(&stack, &bob).await, common::ranked::DEFAULT_RATING);

    let id = mm_match(&stack, 1, &alice, &bob).await;
    // Game 1, Alice wins, reported by both: rated once.
    report(&stack, &alice, &id, 1, Some(&alice)).await;
    report(&stack, &bob, &id, 1, Some(&alice)).await;
    let (a1, b1) = unranked::rate(Standing::new(1700.0), Standing::new(1400.0));
    let (ra, ga) = stored(&stack, &alice).await.unwrap();
    let (rb, gb) = stored(&stack, &bob).await.unwrap();
    assert!(close(ra, a1.rating) && close(rb, b1.rating), "{ra} {rb}");
    assert_eq!((ga, gb), (1, 1));
    // Game 2, Bob wins, only Bob's report: rated on it. Game 3 a draw: not rated.
    report(&stack, &bob, &id, 2, Some(&bob)).await;
    report(&stack, &alice, &id, 3, None).await;
    let (b2, a2) = unranked::rate(b1, a1);
    let (ra, ga) = stored(&stack, &alice).await.unwrap();
    assert!(close(ra, a2.rating), "{ra}");
    assert_eq!(ga, 2);
    assert_eq!(rated_games(&stack).await, 2);
    assert!(close(mm_view(&stack, &bob).await, b2.rating));
    // Ranked sets do not move it, and Direct games are not rated.
    let direct = mm_match(&stack, 2, &alice, &bob).await;
    report(&stack, &alice, &direct, 1, Some(&alice)).await;
    assert_eq!(rated_games(&stack).await, 2);

    // The rebuild replays the same games to the same numbers; the startup backfill sees rated
    // games and does nothing.
    assert_eq!(accounts::unranked::backfill(&stack.pool).await.unwrap(), None);
    let done = accounts::unranked::rebuild(&stack.pool).await.unwrap();
    assert_eq!((done.games, done.players), (2, 2));
    let (ra, ga) = stored(&stack, &alice).await.unwrap();
    let (rb, _) = stored(&stack, &bob).await.unwrap();
    assert!(close(ra, a2.rating) && close(rb, b2.rating), "{ra} {rb}");
    assert_eq!(ga, 2);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_first_start_fills_the_ratings_from_earlier_reports() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "al").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let id = mm_match(&stack, 1, &alice, &bob).await;
    for g in 1..=3 {
        report(&stack, &alice, &id, g, Some(&alice)).await;
    }
    // As before this release: reports stored, nothing rated.
    sqlx::query("DELETE FROM unranked_rated_games").execute(&stack.pool).await.unwrap();
    sqlx::query("DELETE FROM unranked_ratings").execute(&stack.pool).await.unwrap();
    let done = accounts::unranked::backfill(&stack.pool).await.unwrap().expect("filled");
    assert_eq!((done.games, done.players), (3, 2));
    let (ra, ga) = stored(&stack, &alice).await.unwrap();
    assert!(ra > 1500.0, "{ra}");
    assert_eq!(ga, 3);
    assert_eq!(rated_games(&stack).await, 3);
}
