//! End-to-end Ranked: two players meet in mm's Ranked queue, both clients report the games to
//! accounts, the set is settled once, and the Elo ratings come back through `/user/{uid}`, the
//! result endpoint and the next ranked ticket. Also: abandonment, disagreeing reports, the play
//! key check, and a lone report that counts after the grace period.

use std::time::Duration;

use common::ranked::{k_factor, updated, DEFAULT_RATING};
use e2e::{creds, Stack, StackOptions, UserJson};
use mm::engine::EngineConfig;
use mm::ruleset::Rulesets;
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::{json, Value};
use uuid::Uuid;

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

fn engine() -> EngineConfig {
    EngineConfig { rulesets: Rulesets::load(None).unwrap(), ..Default::default() }
}

async fn players(stack: &Stack, names: &[&str]) -> Vec<UserJson> {
    let mut v = vec![];
    for n in names {
        let (_, u) = stack.create_player(&format!("{n}@example.test"), n, &n[..2]).await;
        v.push(u);
    }
    v
}

/// Both search Ranked; returns the match id once mm has recorded it.
async fn ranked_match(stack: &Stack, a: &UserJson, b: &UserJson) -> (String, SearchResult, SearchResult) {
    let (ra, rb) = tokio::join!(
        search(SearchOptions::ranked(stack.mm_addr, creds(a))),
        search(SearchOptions::ranked(stack.mm_addr, creds(b)))
    );
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    let id = ra.ticket_response.as_ref().unwrap().match_id.clone().unwrap();
    assert!(id.starts_with("mode.ranked-"), "{id}");
    for _ in 0..50 {
        let n: i64 = sqlx::query_scalar("SELECT count(*) FROM mm_matches WHERE match_id = $1")
            .bind(&id)
            .fetch_one(&stack.pool)
            .await
            .unwrap();
        if n == 1 {
            return (id, ra, rb);
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    panic!("match {id} was not recorded");
}

/// A ranked set record made directly (as mm would), for the rule tests.
async fn direct_set(stack: &Stack, a: &UserJson, b: &UserJson, n: u32) -> String {
    let id = format!("mode.ranked-test-{n}-{}", Uuid::new_v4());
    let players = [Uuid::parse_str(&a.uid).unwrap(), Uuid::parse_str(&b.uid).unwrap()];
    common::db::insert_match(&stack.pool, &id, 0, &players, players[0], &[], "other", None).await.unwrap();
    id
}

async fn report(stack: &Stack, by: &UserJson, id: &str, game: u32, winner: Option<&UserJson>) -> (u16, Value) {
    stack
        .post(
            "/v1/ranked/report-game",
            None,
            json!({
                "uid": by.uid, "playKey": by.play_key, "matchId": id, "gameIndex": game,
                "winner": winner.map(|w| w.uid.clone()), "stageId": 31, "durationFrames": 9000,
                "players": [{"uid": by.uid, "characterId": 9, "stocksRemaining": 2, "damage": 40.5}],
            }),
        )
        .await
}

async fn both(stack: &Stack, a: &UserJson, b: &UserJson, id: &str, game: u32, winner: &UserJson) -> Value {
    let (st, _) = report(stack, a, id, game, Some(winner)).await;
    assert_eq!(st, 200);
    let (st, body) = report(stack, b, id, game, Some(winner)).await;
    assert_eq!(st, 200, "{body}");
    body
}

async fn leave(stack: &Stack, by: &UserJson, id: &str, kind: &str) -> (u16, Value) {
    stack
        .post(
            "/v1/ranked/report-leave",
            None,
            json!({"uid": by.uid, "playKey": by.play_key, "matchId": id, "kind": kind}),
        )
        .await
}

/// A "left" before the leaver's game was sent to the stage select (bug report #37).
async fn leave_early(stack: &Stack, by: &UserJson, id: &str) -> (u16, Value) {
    stack
        .post(
            "/v1/ranked/report-leave",
            None,
            json!({"uid": by.uid, "playKey": by.play_key, "matchId": id, "kind": "left", "beforeStageSelect": true}),
        )
        .await
}

async fn rank(stack: &Stack, u: &UserJson) -> (f64, u64) {
    let (st, body) = stack.get(&format!("/user/{}?additionalFields=chatMessages,rank", u.uid), None).await;
    assert_eq!(st, 200);
    (body["rank"]["ratingOrdinal"].as_f64().unwrap(), body["rank"]["ratingUpdateCount"].as_u64().unwrap())
}

async fn result(stack: &Stack, u: &UserJson, id: &str) -> Value {
    let (st, body) = stack.get(&format!("/v1/ranked/result?matchId={id}&uid={}", u.uid), None).await;
    assert_eq!(st, 200, "{body}");
    body
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 0.01
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_ranked_set_from_queue_to_rating() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["anna", "boris"]).await;
    let (a, b) = (&u[0], &u[1]);
    assert_eq!(rank(&stack, a).await, (DEFAULT_RATING, 0));

    let (id, ra, _) = ranked_match(&stack, a, b).await;
    // Slippi's ranked response carries each player's rank.
    for p in ra.ticket_response.as_ref().unwrap().players.as_ref().unwrap() {
        let r = p.rank.as_ref().expect("rank in a ranked match");
        assert_eq!((r.rating as f64, r.update_count), (DEFAULT_RATING, 0));
    }

    let s = both(&stack, a, b, &id, 1, a).await;
    assert_eq!(s["status"], "ASSIGNED");
    let ai = s["players"].as_array().unwrap().iter().position(|p| p == &json!(a.uid)).unwrap();
    assert_eq!(s["wins"][ai], 1);
    assert!(s["rating"].is_null());

    // The same report again is fine; another winner for it is not.
    assert_eq!(report(&stack, a, &id, 1, Some(a)).await.0, 200);
    assert_eq!(report(&stack, a, &id, 1, Some(b)).await.0, 409);

    let s = both(&stack, a, b, &id, 2, a).await;
    assert_eq!(s["status"], "COMPLETE", "{s}");
    assert_eq!(s["winner"], json!(a.uid));
    assert_eq!(s["wins"][ai], 2);
    // b asked last: b's change. A first set against an even opponent: K 200, ±100.
    assert!(close(s["rating"]["before"].as_f64().unwrap(), DEFAULT_RATING));
    assert!(close(s["rating"]["change"].as_f64().unwrap(), -k_factor(0) / 2.0), "{s}");
    assert_eq!(s["rating"]["setsPlayed"], 1);

    let r = result(&stack, a, &id).await;
    assert!(close(r["rating"]["after"].as_f64().unwrap(), DEFAULT_RATING + 100.0), "{r}");
    let (ra_, na) = rank(&stack, a).await;
    let (rb_, nb) = rank(&stack, b).await;
    assert!(close(ra_, DEFAULT_RATING + 100.0) && close(rb_, DEFAULT_RATING - 100.0), "{ra_} {rb_}");
    assert_eq!((na, nb), (1, 1));

    // Late reports change nothing: the set is rated once.
    assert_eq!(report(&stack, b, &id, 3, Some(b)).await.0, 200);
    assert_eq!(rank(&stack, a).await.1, 1);
    let events: i64 = sqlx::query_scalar("SELECT count(*) FROM rating_events WHERE match_id = $1")
        .bind(&id)
        .fetch_one(&stack.pool)
        .await
        .unwrap();
    assert_eq!(events, 2);

    // The next ranked ticket carries the new ratings. They are 200 apart, outside ±150 at
    // first; mm's band widens, and the ratings come from the database.
    tokio::time::sleep(Duration::from_secs(2)).await; // the ticket rate limit
    let (_, ra, _) = ranked_match(&stack, a, b).await;
    for p in ra.ticket_response.as_ref().unwrap().players.as_ref().unwrap() {
        let r = p.rank.as_ref().unwrap();
        let want = if p.uid == a.uid { ra_ } else { rb_ };
        assert!(close(r.rating as f64, want) && r.update_count == 1, "{p:?}");
    }
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn abandoning_disagreeing_and_bad_reports() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["carla", "dmitri", "eve"]).await;
    let (a, b, c) = (&u[0], &u[1], &u[2]);

    // Leaving before any game (quitting game 1): the leaver loses, the other player wins.
    let id = direct_set(&stack, a, b, 1).await;
    let (st, s) = leave(&stack, a, &id, "left").await;
    assert_eq!(st, 200);
    assert_eq!(s["status"], "ABANDONED", "{s}");
    assert!(close(s["rating"]["change"].as_f64().unwrap(), -100.0), "{s}");
    let rb = result(&stack, b, &id).await;
    assert_eq!(rb["winner"], json!(b.uid), "{rb}");
    assert!(close(rb["rating"]["change"].as_f64().unwrap(), 100.0), "{rb}");
    let (rating_b, sets_b) = rank(&stack, b).await;
    assert!(close(rating_b, DEFAULT_RATING + 100.0) && sets_b == 1, "{rating_b} {sets_b}");

    // After a game: the one who stays wins the set.
    let id = direct_set(&stack, a, b, 2).await;
    both(&stack, a, b, &id, 1, a).await;
    let (_, s) = leave(&stack, b, &id, "left").await;
    assert_eq!(s["status"], "ABANDONED");
    let ra = result(&stack, a, &id).await;
    let want = updated(DEFAULT_RATING - 100.0, 1, DEFAULT_RATING + 100.0, 1.0);
    assert!(close(ra["rating"]["after"].as_f64().unwrap(), want), "{ra}");

    // Leaving before the stage select with no game played (the search cancelled as the match was
    // made, bug report #37): nobody rated, at once; the opponent's later report changes nothing.
    let before = (rank(&stack, a).await, rank(&stack, b).await);
    let id = direct_set(&stack, a, b, 6).await;
    let (st, s) = leave_early(&stack, a, &id).await;
    assert_eq!((st, s["status"].as_str()), (200, Some("TERMINATED")), "{s}");
    assert!(s["rating"].is_null(), "{s}");
    let (_, s) = leave(&stack, b, &id, "opponent_left").await;
    assert_eq!(s["status"], "TERMINATED");
    assert_eq!((rank(&stack, a).await, rank(&stack, b).await), before);
    // After a game it is an abandon like any other.
    let id = direct_set(&stack, a, b, 7).await;
    both(&stack, a, b, &id, 1, a).await;
    let (_, s) = leave_early(&stack, b, &id).await;
    assert_eq!(s["status"], "ABANDONED", "{s}");

    // A game aborted for its match quality (bug report #182): void at the first report, nobody
    // rated, even after a game one of them won; the other's own report changes nothing.
    let before = (rank(&stack, a).await, rank(&stack, b).await);
    let id = direct_set(&stack, a, b, 8).await;
    both(&stack, a, b, &id, 1, b).await;
    let (st, s) = leave(&stack, a, &id, "match_quality").await;
    assert_eq!((st, s["status"].as_str()), (200, Some("TERMINATED")), "{s}");
    assert!(s["rating"].is_null(), "{s}");
    let (_, s) = leave(&stack, b, &id, "match_quality").await;
    assert_eq!(s["status"], "TERMINATED");
    assert_eq!(result(&stack, b, &id).await["reason"], "match quality low");
    assert_eq!((rank(&stack, a).await, rank(&stack, b).await), before);

    // Both saying the other left: a broken connection, nobody rated.
    let id = direct_set(&stack, a, b, 3).await;
    leave(&stack, a, &id, "opponent_left").await;
    let (_, s) = leave(&stack, b, &id, "opponent_left").await;
    assert_eq!(s["status"], "TERMINATED");
    assert!(s["rating"].is_null());

    // Different winners: held for review, nobody rated.
    let before = (rank(&stack, a).await, rank(&stack, b).await);
    let id = direct_set(&stack, a, b, 4).await;
    report(&stack, a, &id, 1, Some(a)).await;
    let (_, s) = report(&stack, b, &id, 1, Some(b)).await;
    assert_eq!(s["status"], "ERROR", "{s}");
    assert_eq!((rank(&stack, a).await, rank(&stack, b).await), before);

    // The play key, the players and the winner are checked.
    let id = direct_set(&stack, a, b, 5).await;
    let mut forged = a.clone();
    forged.play_key = b.play_key.clone();
    assert_eq!(report(&stack, &forged, &id, 1, Some(a)).await.0, 401);
    assert_eq!(report(&stack, c, &id, 1, Some(c)).await.0, 404);
    assert_eq!(report(&stack, a, &id, 1, Some(c)).await.0, 400);
    assert_eq!(report(&stack, a, &id, 0, Some(a)).await.0, 400);
    assert_eq!(report(&stack, a, "mode.ranked-nope", 1, Some(a)).await.0, 404);
    assert_eq!(leave(&stack, a, &id, "rage_quit").await.0, 400);
    // Not a ranked set: the report is stored, never rated (tests/leaderboard_history.rs), and
    // leaving is for ranked sets only.
    let unranked = format!("mode.unranked-test-{}", Uuid::new_v4());
    let ids = [Uuid::parse_str(&a.uid).unwrap(), Uuid::parse_str(&b.uid).unwrap()];
    common::db::insert_match(&stack.pool, &unranked, 1, &ids, ids[0], &[], "other", None).await.unwrap();
    let (st, s) = report(&stack, a, &unranked, 1, Some(a)).await;
    assert_eq!((st, s["status"].as_str()), (200, Some("ASSIGNED")), "{s}");
    assert!(s["rating"].is_null());
    assert_eq!(leave(&stack, a, &unranked, "left").await.0, 404);
    stack.shutdown().await;
}

/// A game only one client reported counts after the grace period; the background sweep settles it
/// without any further request.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_lone_report_counts_after_the_grace_period() {
    let stack = Stack::start(StackOptions {
        engine: engine(),
        ranked_report_grace_secs: Some(2),
        ranked_sweep_secs: Some(1),
        ..Default::default()
    })
    .await
    .unwrap();
    let u = players(&stack, &["fay", "gus"]).await;
    let (a, b) = (&u[0], &u[1]);
    let id = direct_set(&stack, a, b, 1).await;
    both(&stack, a, b, &id, 1, b).await;
    let (_, s) = report(&stack, b, &id, 2, Some(b)).await;
    assert_eq!(s["status"], "ASSIGNED");
    let mut status = String::new();
    for _ in 0..60 {
        tokio::time::sleep(Duration::from_millis(250)).await;
        status = result(&stack, a, &id).await["status"].as_str().unwrap().to_string();
        if status != "ASSIGNED" {
            break;
        }
    }
    assert_eq!(status, "COMPLETE");
    assert_eq!(result(&stack, b, &id).await["winner"], json!(b.uid));
    assert!(close(rank(&stack, b).await.0, DEFAULT_RATING + 100.0));
    stack.shutdown().await;
}

/// Waits for the background sweep to settle `id`; its status (`ASSIGNED` if it never did).
async fn settled(stack: &Stack, by: &UserJson, id: &str) -> String {
    let mut status = String::new();
    for _ in 0..40 {
        tokio::time::sleep(Duration::from_millis(250)).await;
        status = result(stack, by, id).await["status"].as_str().unwrap().to_string();
        if status != "ASSIGNED" {
            break;
        }
    }
    status
}

fn uid(u: &UserJson) -> Uuid {
    Uuid::parse_str(&u.uid).unwrap()
}

/// 2026-10-21: a player could report their own wins as a set started and have them count after
/// the grace period. Now a game only one player reported waits while the other player's game is
/// connected to mm; that game's own word to mm (`ranked_game_results`) or its leaving mm settles
/// it.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_lone_report_waits_while_the_other_players_game_is_connected() {
    let stack = Stack::start(StackOptions {
        engine: engine(),
        ranked_report_grace_secs: Some(1),
        ranked_sweep_secs: Some(1),
        ..Default::default()
    })
    .await
    .unwrap();
    let u = players(&stack, &["hal", "ivy"]).await;
    let (a, b) = (&u[0], &u[1]);
    let online = |id: String, p: Uuid, on: bool| {
        let pool = stack.pool.clone();
        async move { common::db::record_ranked_presence(&pool, &id, p, on, None, chrono::Utc::now()).await.unwrap() }
    };

    // A reports two wins as the set starts; B's game is connected and playing.
    let id = direct_set(&stack, a, b, 1).await;
    online(id.clone(), uid(a), true).await;
    online(id.clone(), uid(b), true).await;
    report(&stack, a, &id, 1, Some(a)).await;
    report(&stack, a, &id, 2, Some(a)).await;
    assert_eq!(settled(&stack, a, &id).await, "ASSIGNED", "well past the grace period");
    // B's game tells mm game 1 went the other way: held for review, nobody rated.
    common::db::record_ranked_game_result(&stack.pool, &id, 1, uid(b), Some(uid(b)), "game").await.unwrap();
    assert_eq!(settled(&stack, a, &id).await, "ERROR");
    assert_eq!(rank(&stack, a).await.1, 0);

    // B's game agrees with A's lone report: counts without waiting for B's own report.
    let id = direct_set(&stack, a, b, 2).await;
    online(id.clone(), uid(b), true).await;
    both(&stack, a, b, &id, 1, a).await;
    report(&stack, a, &id, 2, Some(a)).await;
    common::db::record_ranked_game_result(&stack.pool, &id, 2, uid(b), Some(uid(a)), "game").await.unwrap();
    assert_eq!(settled(&stack, a, &id).await, "COMPLETE");

    // B's game left mm after the game (closed, crashed): the grace period, as before.
    let id = direct_set(&stack, a, b, 3).await;
    online(id.clone(), uid(a), true).await;
    both(&stack, a, b, &id, 1, b).await;
    report(&stack, b, &id, 2, Some(b)).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    assert_eq!(result(&stack, b, &id).await["status"], "ASSIGNED");
    online(id.clone(), uid(a), false).await;
    assert_eq!(settled(&stack, b, &id).await, "COMPLETE");
    assert_eq!(result(&stack, b, &id).await["winner"], json!(b.uid));

    // "Opponent left" about a player whose game is still connected blames nobody.
    let id = direct_set(&stack, a, b, 4).await;
    online(id.clone(), uid(b), true).await;
    leave(&stack, a, &id, "opponent_left").await;
    assert_eq!(settled(&stack, a, &id).await, "ASSIGNED");
    stack.shutdown().await;
}

/// Bug report #105 (2026-10-22): the opponent's game froze 18 s into game 2 and kept its online
/// connection up for 19 minutes; the other player's "opponent left" waited all that time (and
/// would have been orphaned at 30). Through mm: a game whose `hello` says it sends `game-alive`
/// and that stops sending it while connected is recorded as stopped (`stalled_since`), and an
/// "opponent left" about it made after that abandons the set for it at the next sweep, not after
/// the grace period (here 10 minutes).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn opponent_left_about_a_frozen_game_counts_at_once() {
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;

    let stack = Stack::start(StackOptions {
        engine: EngineConfig { game_stall: Duration::from_secs(2), ..engine() },
        ranked_report_grace_secs: Some(600),
        ranked_sweep_secs: Some(1),
        ..Default::default()
    })
    .await
    .unwrap();
    let u = players(&stack, &["lou", "max"]).await;
    let (a, b) = (&u[0], &u[1]);
    // Max's game: online, kept serviced, sending game-alive while `running`.
    let (mut game, hello) =
        mmclient::room::OnlineClient::connect_game_alive(stack.mm_addr, &creds(b), "0.1.0").unwrap();
    assert_eq!(hello["type"], "hello-resp");
    let (running, stop) = (Arc::new(AtomicBool::new(true)), Arc::new(AtomicBool::new(false)));
    let pump = {
        let (running, stop) = (running.clone(), stop.clone());
        std::thread::spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                if running.load(Ordering::Relaxed) {
                    game.send_game_alive();
                }
                game.drain(Duration::from_millis(250));
            }
        })
    };
    let (id, _, _) = ranked_match(&stack, a, b).await;
    both(&stack, a, b, &id, 1, a).await;
    let stalled = || async {
        let row: Option<Option<chrono::DateTime<chrono::Utc>>> =
            sqlx::query_scalar("SELECT stalled_since FROM ranked_presence WHERE match_id = $1 AND uid = $2")
                .bind(&id)
                .bind(uid(b))
                .fetch_optional(&stack.pool)
                .await
                .unwrap();
        row.flatten()
    };
    // Running for longer than game_stall: not stopped.
    tokio::time::sleep(Duration::from_secs(3)).await;
    assert_eq!(stalled().await, None);
    // Frozen: its connection stays up, its game-alive stops.
    running.store(false, Ordering::Relaxed);
    let mut since = None;
    for _ in 0..40 {
        tokio::time::sleep(Duration::from_millis(250)).await;
        since = stalled().await;
        if since.is_some() {
            break;
        }
    }
    assert!(since.is_some(), "max's game was never recorded as stopped");
    let (st, _) = leave(&stack, a, &id, "opponent_left").await;
    assert_eq!(st, 200);
    assert_eq!(settled(&stack, a, &id).await, "ABANDONED");
    let r = result(&stack, a, &id).await;
    assert_eq!(r["winner"], json!(a.uid), "{r}");
    stop.store(true, Ordering::Relaxed);
    pump.join().unwrap();
    stack.shutdown().await;
}
