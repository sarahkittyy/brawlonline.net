//! The 2v2 queue's hidden rating end to end (`docs/design/2v2.md`): a 2v2 game's first report with
//! a winner rates it once, every player from their Unranked rating at their first game, a pair
//! that queued together expected to do better, a leaver with stocks losing; mm's account query
//! reads it.

use common::db;
use common::twos::{self, Player};
use common::unranked::Standing;
use e2e::{Stack, StackOptions, UserJson};
use serde_json::{json, Value};
use uuid::Uuid;

fn uuid(u: &UserJson) -> Uuid {
    u.uid.parse().unwrap()
}

async fn report(stack: &Stack, by: &UserJson, id: &str, game: u32, winner: &UserJson, players: Value) {
    let (st, body) = stack
        .post(
            "/v1/ranked/report-game",
            None,
            json!({
                "uid": by.uid, "playKey": by.play_key, "matchId": id, "gameIndex": game,
                "winner": winner.uid, "stageId": 31, "durationFrames": 9000, "players": players,
            }),
        )
        .await;
    assert_eq!(st, 200, "{body}");
}

async fn stored(stack: &Stack, u: &UserJson) -> Option<(f64, i32)> {
    sqlx::query_as("SELECT rating, games FROM twos_ratings WHERE uid = $1")
        .bind(uuid(u))
        .fetch_optional(&stack.pool)
        .await
        .unwrap()
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 1e-6
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn twos_games_move_the_hidden_rating_once_each() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let mut p = Vec::new();
    for (i, name) in ["ann", "ben", "cat", "dan"].iter().enumerate() {
        let (_, u) = stack.create_player(&format!("{name}@example.test"), name, ["ta", "tb", "tc", "td"][i]).await;
        p.push(u);
    }
    // Ann has an Unranked rating (1600), Ben only a Ranked one (1500), Cat and Dan neither.
    sqlx::query("INSERT INTO ratings (uid, rating, sets_played) VALUES ($1, 1700, 12), ($2, 1500, 9)")
        .bind(uuid(&p[0]))
        .bind(uuid(&p[1]))
        .execute(&stack.pool)
        .await
        .unwrap();
    sqlx::query("INSERT INTO unranked_ratings (uid, rating, games) VALUES ($1, 1600, 40)")
        .bind(uuid(&p[0]))
        .execute(&stack.pool)
        .await
        .unwrap();
    // mm sees the Unranked rating before any 2v2 game (else Ranked's, else the default).
    let view = |u: Uuid| {
        let pool = stack.pool.clone();
        async move { db::fetch_mm_user(&pool, u).await.unwrap().unwrap().twos_rating }
    };
    assert_eq!(view(uuid(&p[0])).await, 1600.0);
    assert_eq!(view(uuid(&p[1])).await, 1500.0);
    assert_eq!(view(uuid(&p[2])).await, common::ranked::DEFAULT_RATING);

    // Ann and Ben queued together (pair 1); Cat and Dan alone.
    let uids: Vec<Uuid> = p.iter().map(uuid).collect();
    let id = format!("mode.twos-test-{}", Uuid::new_v4().simple());
    db::insert_twos_match(&stack.pool, &id, &uids, uids[0], &[], "other", &[1, 1, 0, 0]).await.unwrap();
    let game1 = json!([
        {"uid": p[0].uid, "port": 1, "team": 0, "place": 1},
        {"uid": p[1].uid, "port": 2, "team": 0, "place": 1},
        {"uid": p[2].uid, "port": 3, "team": 1, "place": 2},
        {"uid": p[3].uid, "port": 4, "team": 1, "place": 2},
    ]);
    // Game 1, Ann and Ben win, reported by everyone: rated once.
    for by in &p {
        report(&stack, by, &id, 1, &p[0], game1.clone()).await;
    }
    let before = |r: f64| Standing { rating: r, games: 0 };
    let d = common::ranked::DEFAULT_RATING;
    let seat = |i: usize, r: f64, team: u8| Player { uid: uids[i], before: before(r), team, left: false, eliminated: false };
    let want = twos::rate(&[seat(0, 1600.0, 0), seat(1, 1500.0, 0), seat(2, d, 1), seat(3, d, 1)], [true, false], 0)
        .unwrap();
    for (i, w) in want.iter().enumerate() {
        let (r, g) = stored(&stack, &p[i]).await.unwrap();
        assert!(close(r, w.rating), "{i}: {r} vs {}", w.rating);
        assert_eq!(g, 1);
    }
    assert_eq!(view(uuid(&p[0])).await, want[0].rating);
    let n: i64 = sqlx::query_scalar("SELECT count(*) FROM twos_rated_games").fetch_one(&stack.pool).await.unwrap();
    assert_eq!(n, 1);

    // Game 2, the teams shuffled: Ann with Cat, Ben with Dan. Ben leaves with stocks, Dan wins the
    // 1v2 anyway: Ben loses, Dan wins. Nobody on a team queued together.
    let game2 = json!([
        {"uid": p[0].uid, "port": 1, "team": 0, "place": 2},
        {"uid": p[1].uid, "port": 2, "team": 1, "left": true},
        {"uid": p[2].uid, "port": 3, "team": 0, "place": 2},
        {"uid": p[3].uid, "port": 4, "team": 1, "place": 1},
    ]);
    report(&stack, &p[3], &id, 2, &p[3], game2).await;
    let now = |i: usize, team: u8, left: bool| Player {
        uid: uids[i],
        before: Standing { rating: want[i].rating, games: 1 },
        team,
        left,
        eliminated: false,
    };
    let want2 = twos::rate(&[now(0, 0, false), now(1, 1, true), now(2, 0, false), now(3, 1, false)], [false, false], 1)
        .unwrap();
    let (ben, _) = stored(&stack, &p[1]).await.unwrap();
    let (dan, _) = stored(&stack, &p[3]).await.unwrap();
    assert!(close(ben, want2[1].rating) && ben < want[1].rating, "the leaver loses");
    assert!(close(dan, want2[3].rating) && dan > want[3].rating, "the one who stayed wins");

    // A report that is not two on two is stored but not rated.
    let three = json!([
        {"uid": p[0].uid, "team": 0}, {"uid": p[1].uid, "team": 0}, {"uid": p[2].uid, "team": 0}, {"uid": p[3].uid, "team": 1},
    ]);
    report(&stack, &p[0], &id, 3, &p[0], three).await;
    let n: i64 = sqlx::query_scalar("SELECT count(*) FROM twos_rated_games").fetch_one(&stack.pool).await.unwrap();
    assert_eq!(n, 2);
}
