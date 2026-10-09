//! End-to-end: the ranked leaderboard (pages, positions, ties, cursors that stay put while
//! ratings change), the leaderboard position in `/user/{uid}`, a player's match history
//! (Ranked, Unranked, Direct, an abandoned set; filters, fields, pages), game reports for
//! Unranked and Direct matches (stored, never rated), and the rate limits of both endpoints.

use chrono::{DateTime, Duration, Utc};
use common::ranked::{updated, DEFAULT_RATING};
use e2e::{Stack, StackOptions, UserJson};
use serde_json::{json, Value};
use sqlx::PgPool;
use uuid::Uuid;

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() < 0.01
}

// ---------------------------------------------------------------- leaderboard

/// A rated player written straight to the database (the leaderboard reads only `users` and
/// `ratings`).
async fn rated(pool: &PgPool, n: usize, rating: Option<f64>, sets: i32) -> Uuid {
    let uid = Uuid::new_v4();
    sqlx::query("INSERT INTO users (uid, email, pw_hash, display_name, connect_code) VALUES ($1, $2, 'x', $3, $4)")
        .bind(uid)
        .bind(format!("p{n}@example.test"))
        .bind(format!("player{n}"))
        .bind(format!("P#{n}"))
        .execute(pool)
        .await
        .unwrap();
    if let Some(r) = rating {
        sqlx::query("INSERT INTO ratings (uid, rating, sets_played, wins, losses) VALUES ($1, $2, $3, $4, $5)")
            .bind(uid)
            .bind(r)
            .bind(sets)
            .bind(sets / 2)
            .bind(sets - sets / 2)
            .execute(pool)
            .await
            .unwrap();
    }
    uid
}

/// Every rated player in leaderboard order: rating, highest first, then uid.
async fn expected_order(pool: &PgPool) -> Vec<(Uuid, f64)> {
    let mut rows: Vec<(Uuid, f64)> =
        sqlx::query_as("SELECT uid, rating FROM ratings WHERE sets_played > 0").fetch_all(pool).await.unwrap();
    rows.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap().then(a.0.cmp(&b.0)));
    rows
}

async fn board(stack: &Stack, query: &str) -> Value {
    let (st, body) = stack.get(&format!("/v1/ranked/leaderboard{query}"), None).await;
    assert_eq!(st, 200, "{query}: {body}");
    body
}

/// All pages of `limit`, following `next` until null.
async fn all_pages(stack: &Stack, limit: u32) -> (Vec<Value>, usize) {
    let mut entries = vec![];
    let mut pages = 0;
    let mut q = format!("?limit={limit}");
    loop {
        let page = board(stack, &q).await;
        pages += 1;
        let got = page["entries"].as_array().unwrap();
        assert!(got.len() <= limit as usize);
        entries.extend(got.iter().cloned());
        match page["next"].as_str() {
            Some(c) => {
                assert_eq!(got.len(), limit as usize, "a page before the last is full");
                q = format!("?limit={limit}&after={c}");
            }
            None => return (entries, pages),
        }
        assert!(pages < 100);
    }
}

fn uid_of(e: &Value) -> Uuid {
    Uuid::parse_str(e["uid"].as_str().unwrap()).unwrap()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn leaderboard_pages_positions_and_ties() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    // 25 rated players in groups of three with the same rating, one player whose ratings row
    // has no rated set, and one with no row: neither is on the leaderboard.
    for n in 0..25 {
        rated(&stack.pool, n, Some(1700.0 - (n / 3) as f64 * 12.5), (n % 4 + 1) as i32).await;
    }
    let unrated_row = rated(&stack.pool, 100, Some(1400.0), 0).await;
    let no_row = rated(&stack.pool, 101, None, 0).await;
    let want = expected_order(&stack.pool).await;
    assert_eq!(want.len(), 25);

    // Default page size (50) holds everyone.
    let first = board(&stack, "").await;
    assert_eq!(first["total"], 25);
    assert!(first["next"].is_null());
    let e = &first["entries"][0];
    assert_eq!(e["position"], 1);
    for key in ["uid", "displayName", "connectCode", "rating", "setsPlayed", "wins", "losses"] {
        assert!(!e[key].is_null(), "{key} in {e}");
    }
    // The top three share a rating: the lowest uid comes first.
    assert_eq!(uid_of(e), want[0].0);
    assert!(e["connectCode"].as_str().unwrap().starts_with("P#"));
    assert_eq!(e["setsPlayed"].as_u64().unwrap(), e["wins"].as_u64().unwrap() + e["losses"].as_u64().unwrap());

    // Pages of 7: 7 + 7 + 7 + 4, positions 1..=25 strictly increasing across the pages, ties
    // ordered by uid.
    let (entries, pages) = all_pages(&stack, 7).await;
    assert_eq!(pages, 4);
    let got: Vec<(Uuid, f64)> = entries.iter().map(|e| (uid_of(e), e["rating"].as_f64().unwrap())).collect();
    assert_eq!(got, want);
    let positions: Vec<u64> = entries.iter().map(|e| e["position"].as_u64().unwrap()).collect();
    assert_eq!(positions, (1..=25).collect::<Vec<u64>>());
    assert!(!got.iter().any(|(u, _)| *u == unrated_row || *u == no_row));
    // A page size that divides the total: the last page is full and has no `next`.
    let (entries5, pages5) = all_pages(&stack, 5).await;
    assert_eq!((entries5.len(), pages5), (25, 5));
    // limit is at least 1.
    assert_eq!(board(&stack, "?limit=0").await["entries"].as_array().unwrap().len(), 1);

    // `/user/{uid}` places every player the same way.
    for (i, (uid, _)) in want.iter().enumerate().step_by(4) {
        let (_, u) = stack.get(&format!("/user/{uid}?additionalFields=chatMessages,rank"), None).await;
        assert_eq!(u["rank"]["position"], i as u64 + 1, "{u}");
        assert_eq!(u["rank"]["rankedPlayers"], 25);
    }
    for uid in [unrated_row, no_row] {
        let (_, u) = stack.get(&format!("/user/{uid}"), None).await;
        assert!(u["rank"]["position"].is_null(), "{u}");
        assert_eq!(u["rank"]["rankedPlayers"], 25);
        // The existing fields are still there.
        assert_eq!(u["rank"]["ratingUpdateCount"], 0);
        assert!(u["rank"]["ratingOrdinal"].is_number());
    }

    // The cursor stays put while ratings change: after page 1, the 20th player jumps to the
    // top and the 2nd drops to the bottom. Page 2 continues right after page 1's last row
    // (rating, uid), so nothing is skipped or repeated relative to that row, and its positions
    // are those of the new order.
    let p1 = board(&stack, "?limit=7").await;
    let cursor = p1["next"].as_str().unwrap().to_string();
    let last = (uid_of(&p1["entries"][6]), p1["entries"][6]["rating"].as_f64().unwrap());
    let (jumper, dropper) = (want[19].0, want[1].0);
    sqlx::query("UPDATE ratings SET rating = 2500 WHERE uid = $1").bind(jumper).execute(&stack.pool).await.unwrap();
    sqlx::query("UPDATE ratings SET rating = 900 WHERE uid = $1").bind(dropper).execute(&stack.pool).await.unwrap();
    let now = expected_order(&stack.pool).await;
    let after: Vec<(usize, Uuid)> = now
        .iter()
        .enumerate()
        .filter(|(_, (u, r))| *r < last.1 || (*r == last.1 && *u > last.0))
        .map(|(i, (u, _))| (i, *u))
        .take(7)
        .collect();
    let p2 = board(&stack, &format!("?limit=7&after={cursor}")).await;
    let p2_rows: Vec<(usize, Uuid)> = p2["entries"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| (e["position"].as_u64().unwrap() as usize - 1, uid_of(e)))
        .collect();
    assert_eq!(p2_rows, after);
    assert!(!p2_rows.iter().any(|(_, u)| *u == jumper));
    // The jumper is now ahead of the cursor, the dropper behind: one each way, so page 2 still
    // starts at position 8.
    assert_eq!(p2_rows[0].0, 7);
    // The same cursor again: the same page.
    assert_eq!(board(&stack, &format!("?limit=7&after={cursor}")).await["entries"], p2["entries"]);

    // Bad cursors.
    for bad in ["nope", "aDE6eA", &cursor[..cursor.len() - 3]] {
        let (st, body) = stack.get(&format!("/v1/ranked/leaderboard?after={bad}"), None).await;
        assert_eq!(st, 400, "{bad}: {body}");
        assert_eq!(body["error"]["code"], "invalid_cursor");
    }
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn leaderboard_and_history_are_rate_limited() {
    let stack = Stack::start(StackOptions { rate_limits: true, trust_proxy_headers: true, ..Default::default() })
        .await
        .unwrap();
    let get = |ip: String| {
        let req = stack.http.get(stack.url("/v1/ranked/leaderboard")).header("x-forwarded-for", ip);
        async move {
            let resp = req.send().await.unwrap();
            (
                resp.status().as_u16(),
                resp.headers().get("retry-after").and_then(|v| v.to_str().ok()?.parse::<u64>().ok()),
            )
        }
    };
    // 30 a minute per IP.
    for _ in 0..30 {
        assert_eq!(get("203.0.113.5".into()).await.0, 200);
    }
    let (st, retry) = get("203.0.113.5".into()).await;
    assert_eq!(st, 429);
    assert!(retry.is_some_and(|s| (1..=60).contains(&s)), "{retry:?}");
    assert_eq!(get("203.0.113.6".into()).await.0, 200);
    // An IPv6 /64 is one client.
    for i in 0..30 {
        assert_eq!(get(format!("2001:db8:5:6::{:x}", i + 1)).await.0, 200);
    }
    assert_eq!(get("2001:db8:5:6:ffff::1".into()).await.0, 429);
    assert_eq!(get("2001:db8:5:7::1".into()).await.0, 200);

    // Match history: 60 a minute per account.
    let (session, _) = stack.create_player("rl@example.test", "rl", "RL").await;
    for _ in 0..60 {
        assert_eq!(stack.get("/v1/me/matches", Some(&session)).await.0, 200);
    }
    let (st, body) = stack.get("/v1/me/matches", Some(&session)).await;
    assert_eq!(st, 429, "{body}");
    assert_eq!(body["error"]["code"], "rate_limited");
    stack.shutdown().await;
}

// ---------------------------------------------------------------- history

async fn players(stack: &Stack, names: &[&str]) -> Vec<(String, UserJson)> {
    let mut v = vec![];
    for n in names {
        v.push(stack.create_player(&format!("{n}@example.test"), n, &n[..2]).await);
    }
    v
}

fn uuid(u: &UserJson) -> Uuid {
    Uuid::parse_str(&u.uid).unwrap()
}

/// A match as mm records it, made `minutes` after `t0`.
async fn mm_match(stack: &Stack, mode: i16, a: &UserJson, b: &UserJson, t0: DateTime<Utc>, minutes: i64) -> String {
    let kind = ["ranked", "unranked", "direct"][mode as usize];
    let id = format!("mode.{kind}-test-{minutes}-{}", Uuid::new_v4().simple());
    let p = [uuid(a), uuid(b)];
    common::db::insert_match(&stack.pool, &id, mode, &p, p[0], &[], "other").await.unwrap();
    sqlx::query("UPDATE mm_matches SET created_at = $2 WHERE match_id = $1")
        .bind(&id)
        .bind(t0 + Duration::minutes(minutes))
        .execute(&stack.pool)
        .await
        .unwrap();
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

async fn ok_report(stack: &Stack, by: &UserJson, id: &str, game: u32, winner: Option<&UserJson>) -> Value {
    let (st, body) = report(stack, by, id, game, winner).await;
    assert_eq!(st, 200, "{body}");
    body
}

async fn history(stack: &Stack, session: &str, query: &str) -> Value {
    let (st, body) = stack.get(&format!("/v1/me/matches{query}"), Some(session)).await;
    assert_eq!(st, 200, "{query}: {body}");
    body
}

fn ids(page: &Value) -> Vec<String> {
    page["matches"].as_array().unwrap().iter().map(|m| m["matchId"].as_str().unwrap().to_string()).collect()
}

fn item<'a>(page: &'a Value, id: &str) -> &'a Value {
    page["matches"].as_array().unwrap().iter().find(|m| m["matchId"] == id).unwrap()
}

fn player<'a>(m: &'a Value, u: &UserJson) -> &'a Value {
    m["players"].as_array().unwrap().iter().find(|p| p["uid"] == u.uid.as_str()).unwrap()
}

async fn rating_events(pool: &PgPool, id: &str) -> i64 {
    sqlx::query_scalar("SELECT count(*) FROM rating_events WHERE match_id = $1").bind(id).fetch_one(pool).await.unwrap()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn match_history_and_unrated_reports() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let u = players(&stack, &["hana", "ivan", "jules"]).await;
    let (sa, a) = (&u[0].0, &u[0].1);
    let (sb, b) = (&u[1].0, &u[1].1);
    let c = &u[2].1;
    let t0 = Utc::now() - Duration::minutes(20);

    // 1. Ranked, a wins 2-0.
    let m1 = mm_match(&stack, 0, a, b, t0, 1).await;
    for g in 1..=2 {
        ok_report(&stack, a, &m1, g, Some(a)).await;
        ok_report(&stack, b, &m1, g, Some(a)).await;
    }
    // 2. Ranked, b leaves before any game: b loses rating, a gets nothing.
    let m2 = mm_match(&stack, 0, a, b, t0, 2).await;
    let (st, s) = stack
        .post(
            "/v1/ranked/report-leave",
            None,
            json!({"uid": b.uid, "playKey": b.play_key, "matchId": m2, "kind": "left"}),
        )
        .await;
    assert_eq!((st, s["status"].as_str()), (200, Some("ABANDONED")), "{s}");
    let (rating_a, rating_b) = (DEFAULT_RATING + 100.0, DEFAULT_RATING - 100.0);

    // 3. Unranked: agreed, lone, drawn, disagreeing and a late (game 15) report.
    let m3 = mm_match(&stack, 1, a, b, t0, 3).await;
    let s = ok_report(&stack, a, &m3, 1, Some(a)).await;
    assert_eq!(s["status"], "ASSIGNED");
    assert_eq!(s["wins"], json!([1, 0]));
    assert!(s["rating"].is_null() && s["winner"].is_null());
    ok_report(&stack, b, &m3, 1, Some(a)).await;
    ok_report(&stack, b, &m3, 2, Some(b)).await;
    ok_report(&stack, a, &m3, 3, None).await;
    ok_report(&stack, a, &m3, 4, Some(a)).await;
    ok_report(&stack, b, &m3, 4, Some(b)).await;
    ok_report(&stack, a, &m3, 15, Some(a)).await;
    let s = ok_report(&stack, b, &m3, 15, Some(a)).await;
    assert_eq!(s["status"], "ASSIGNED");
    assert_eq!(s["players"], json!([a.uid, b.uid]));
    assert_eq!(s["wins"], json!([2, 1]));
    // Same checks as Ranked: idempotent, another winner refused, strangers, bad winners and
    // indexes; leave reports and the result endpoint stay Ranked-only.
    assert_eq!(report(&stack, a, &m3, 1, Some(a)).await.0, 200);
    assert_eq!(report(&stack, a, &m3, 1, Some(b)).await.0, 409);
    assert_eq!(report(&stack, c, &m3, 5, Some(c)).await.0, 404);
    assert_eq!(report(&stack, a, &m3, 5, Some(c)).await.0, 400);
    assert_eq!(report(&stack, a, &m3, 0, Some(a)).await.0, 400);
    assert_eq!(report(&stack, a, &m3, 1000, Some(a)).await.0, 400);
    let mut forged = a.clone();
    forged.play_key = b.play_key.clone();
    assert_eq!(report(&stack, &forged, &m3, 5, Some(a)).await.0, 401);
    let (st, _) = stack
        .post(
            "/v1/ranked/report-leave",
            None,
            json!({"uid": a.uid, "playKey": a.play_key, "matchId": m3, "kind": "left"}),
        )
        .await;
    assert_eq!(st, 404);
    assert_eq!(stack.get(&format!("/v1/ranked/result?matchId={m3}&uid={}", a.uid), None).await.0, 404);

    // 4. Direct, a vs c, one game reported by c only.
    let m4 = mm_match(&stack, 2, a, c, t0, 4).await;
    let s = ok_report(&stack, c, &m4, 1, Some(a)).await;
    assert_eq!(s["wins"], json!([1, 0]));
    // 5. Unranked with no report (a failed connect): not in the history.
    let m5 = mm_match(&stack, 1, a, b, t0, 5).await;
    // 6. Ranked that went silent (ORPHANED, nobody rated, no report): not in the history.
    let m6 = mm_match(&stack, 0, a, b, t0, 6).await;
    sqlx::query("UPDATE mm_matches SET status = 'ORPHANED', ended_at = now() WHERE match_id = $1")
        .bind(&m6)
        .execute(&stack.pool)
        .await
        .unwrap();
    // 7. Ranked, in progress after one game.
    let m7 = mm_match(&stack, 0, b, a, t0, 7).await;
    ok_report(&stack, a, &m7, 1, Some(b)).await;
    ok_report(&stack, b, &m7, 1, Some(b)).await;
    // 8. b vs c: not a's.
    let m8 = mm_match(&stack, 1, b, c, t0, 8).await;
    ok_report(&stack, b, &m8, 1, Some(c)).await;

    // Unranked and Direct reports were never rated, and the sweep leaves them alone.
    for id in [&m3, &m4, &m8] {
        assert_eq!(rating_events(&stack.pool, id).await, 0);
        let status: String = sqlx::query_scalar("SELECT status FROM mm_matches WHERE match_id = $1")
            .bind(id)
            .fetch_one(&stack.pool)
            .await
            .unwrap();
        assert_eq!(status, "ASSIGNED");
    }
    assert_eq!(
        accounts::ranked::sweep(&stack.pool, Utc::now() + Duration::hours(2), Default::default()).await.unwrap(),
        1
    );
    let (_, uc) = stack.get(&format!("/user/{}", c.uid), None).await;
    assert_eq!(uc["rank"]["ratingUpdateCount"], 0);
    assert!(uc["rank"]["position"].is_null());
    assert_eq!(uc["rank"]["rankedPlayers"], 2);
    // The sweep closed m7 as ORPHANED after its stale time; it stays in the history (a game
    // was reported). Positions: a first, b second.
    let (_, ua) = stack.get(&format!("/user/{}", a.uid), None).await;
    assert_eq!((ua["rank"]["position"].as_u64(), ua["rank"]["rankedPlayers"].as_u64()), (Some(1), Some(2)));
    let (_, ub) = stack.get(&format!("/user/{}", b.uid), None).await;
    assert_eq!(ub["rank"]["position"], 2);

    // a's history, newest first.
    let all = history(&stack, sa, "").await;
    assert_eq!(ids(&all), vec![m7.clone(), m4.clone(), m3.clone(), m2.clone(), m1.clone()]);
    assert!(all["next"].is_null());
    assert!(!ids(&all).contains(&m5) && !ids(&all).contains(&m6) && !ids(&all).contains(&m8));

    let r1 = item(&all, &m1);
    assert_eq!((r1["mode"].as_str(), r1["ranked"].as_bool()), (Some("ranked"), Some(true)));
    assert_eq!(r1["status"], "COMPLETE");
    assert_eq!(r1["winner"], a.uid.as_str());
    assert!(r1["endReason"].is_null());
    let created = DateTime::parse_from_rfc3339(r1["createdAt"].as_str().unwrap()).unwrap();
    assert!((created.with_timezone(&Utc) - (t0 + Duration::minutes(1))).num_milliseconds().abs() < 1);
    let (pa, pb) = (player(r1, a), player(r1, b));
    assert_eq!((pa["wins"].as_u64(), pb["wins"].as_u64()), (Some(2), Some(0)));
    assert_eq!(pa["displayName"], "hana");
    assert_eq!(pa["connectCode"], a.connect_code.as_str());
    assert!(close(pa["ratingBefore"].as_f64().unwrap(), DEFAULT_RATING));
    assert!(close(pa["ratingAfter"].as_f64().unwrap(), rating_a));
    assert!(close(pa["ratingChange"].as_f64().unwrap(), 100.0));
    assert!(close(pb["ratingChange"].as_f64().unwrap(), -100.0));

    let r2 = item(&all, &m2);
    assert_eq!(r2["status"], "ABANDONED");
    assert!(r2["winner"].is_null());
    assert!(r2["endReason"].as_str().unwrap().starts_with("abandoned by"), "{r2}");
    let (pa, pb) = (player(r2, a), player(r2, b));
    assert_eq!((pa["wins"].as_u64(), pb["wins"].as_u64()), (Some(0), Some(0)));
    assert!(pa["ratingBefore"].is_null() && pa["ratingChange"].is_null());
    assert!(close(pb["ratingBefore"].as_f64().unwrap(), rating_b));
    let want = updated(rating_b, 1, rating_a, 0.0) - rating_b;
    assert!(close(pb["ratingChange"].as_f64().unwrap(), want), "{pb}");

    let r3 = item(&all, &m3);
    assert_eq!((r3["mode"].as_str(), r3["ranked"].as_bool()), (Some("unranked"), Some(false)));
    assert_eq!(r3["status"], "ASSIGNED");
    assert!(r3["winner"].is_null() && r3["endReason"].is_null());
    assert_eq!((player(r3, a)["wins"].as_u64(), player(r3, b)["wins"].as_u64()), (Some(2), Some(1)));
    assert!(player(r3, a)["ratingBefore"].is_null() && player(r3, a)["ratingAfter"].is_null());

    let r4 = item(&all, &m4);
    assert_eq!((r4["mode"].as_str(), r4["ranked"].as_bool()), (Some("direct"), Some(false)));
    assert_eq!((player(r4, a)["wins"].as_u64(), player(r4, c)["wins"].as_u64()), (Some(1), Some(0)));
    assert_eq!(player(r4, c)["displayName"], "jules");

    let r7 = item(&all, &m7);
    assert_eq!(r7["status"], "ORPHANED");
    assert_eq!(r7["players"][0]["uid"], b.uid.as_str(), "the players in mm's order");
    assert_eq!((player(r7, b)["wins"].as_u64(), player(r7, a)["wins"].as_u64()), (Some(1), Some(0)));
    assert!(player(r7, a)["ratingChange"].is_null());

    // Filters: ranked; unranked means Unranked and Direct.
    assert_eq!(ids(&history(&stack, sa, "?mode=ranked").await), vec![m7.clone(), m2.clone(), m1.clone()]);
    assert_eq!(ids(&history(&stack, sa, "?mode=unranked").await), vec![m4.clone(), m3.clone()]);
    assert_eq!(ids(&history(&stack, sa, "?mode=all").await), ids(&all));
    // b's own history.
    assert_eq!(ids(&history(&stack, sb, "").await), vec![m8.clone(), m7.clone(), m3.clone(), m2.clone(), m1.clone()]);

    // Pages of 2, and a match made after page 1 does not move page 2.
    let p1 = history(&stack, sa, "?limit=2").await;
    assert_eq!(ids(&p1), vec![m7.clone(), m4.clone()]);
    let cursor = p1["next"].as_str().unwrap().to_string();
    let newer = mm_match(&stack, 1, a, b, t0, 9).await;
    ok_report(&stack, a, &newer, 1, Some(a)).await;
    let p2 = history(&stack, sa, &format!("?limit=2&before={cursor}")).await;
    assert_eq!(ids(&p2), vec![m3.clone(), m2.clone()]);
    let p3 = history(&stack, sa, &format!("?limit=2&before={}", p2["next"].as_str().unwrap())).await;
    assert_eq!(ids(&p3), vec![m1.clone()]);
    assert!(p3["next"].is_null());
    assert_eq!(ids(&history(&stack, sa, "?limit=1").await), vec![newer]);
    let ranked_p1 = history(&stack, sa, "?mode=ranked&limit=1").await;
    let ranked_p2 =
        history(&stack, sa, &format!("?mode=ranked&limit=2&before={}", ranked_p1["next"].as_str().unwrap())).await;
    assert_eq!(ids(&ranked_p2), vec![m2.clone(), m1.clone()]);
    assert!(ranked_p2["next"].is_null());

    // Session auth, bad parameters.
    assert_eq!(stack.get("/v1/me/matches", None).await.0, 401);
    assert_eq!(stack.get("/v1/me/matches", Some("nope")).await.0, 401);
    let (st, body) = stack.get("/v1/me/matches?mode=teams", Some(sa)).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_mode")));
    let (st, body) = stack.get("/v1/me/matches?before=xyz", Some(sa)).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_cursor")));
    // A leaderboard cursor is not a history cursor.
    let lb = board(&stack, "?limit=1").await;
    let (st, _) = stack.get(&format!("/v1/me/matches?before={}", lb["next"].as_str().unwrap()), Some(sa)).await;
    assert_eq!(st, 400);
    stack.shutdown().await;
}
