//! End-to-end: the builds of the game (`docs/design/pplus-mods.md`, `mm::games`). Postgres +
//! accounts (HTTP) + mm (ENet/UDP) + fake game clients that name the build their Dolphin runs
//! (`hello.game`, a ticket's `game`): a mod of P+ meets only its own build in the queue and in
//! rooms, plays its own stage list, is recorded with its build, and leaves P+'s hidden Unranked
//! rating alone.

use std::time::Duration;

use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mm::engine::EngineConfig;
use mm::messages as msg;
use mm::ruleset::Rulesets;
use mmclient::room::OnlineClient;
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::{json, Value};

const COBALT: &str = "Cobalt:fe22b89a8cf4cdaac89bc39227111c9f";
/// Cobalt 1.34's stages (`config/games.json`): its five starters, their partners, its counterpicks.
const COBALT_STAGES: [u16; 15] = [1, 46, 33, 25, 74, 19, 42, 7, 4, 2, 76, 12, 35, 68, 30];
const WAIT: Duration = Duration::from_secs(5);

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

fn engine() -> EngineConfig {
    EngineConfig { rulesets: Rulesets::load(None).unwrap(), ..Default::default() }
}

fn unranked(stack: &Stack, u: &UserJson, game: &str, timeout_secs: u64) -> SearchOptions {
    let mut o = SearchOptions::unranked(stack.mm_addr, creds(u));
    o.game = game.into();
    o.match_timeout = Some(Duration::from_secs(timeout_secs));
    o
}

async fn players(stack: &Stack, names: &[&str]) -> Vec<(String, UserJson)> {
    let mut v = vec![];
    for n in names {
        v.push(stack.create_player(&format!("{n}@example.test"), n, &n[..2]).await);
    }
    v
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_mod_meets_only_its_own_build_in_unranked() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["anna", "boris", "clara"]).await;

    // Cobalt and P+ search at once: neither finds the other.
    let (ra, rb) = tokio::join!(search(unranked(&stack, &u[0].1, COBALT, 3)), search(unranked(&stack, &u[1].1, "", 3)));
    assert_eq!(ra.status, Status::MatchTimeout, "{ra:?}");
    assert_eq!(rb.status, Status::MatchTimeout, "{rb:?}");

    // Two Cobalt players: matched, on Cobalt's stages.
    let (ra, rc) =
        tokio::join!(search(unranked(&stack, &u[0].1, COBALT, 20)), search(unranked(&stack, &u[2].1, COBALT, 20)));
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rc.status, Status::Matched, "{rc:?}");
    let ta = ra.ticket_response.as_ref().unwrap();
    let match_id = ta.match_id.clone().unwrap();
    assert_eq!(ta.stages.as_deref(), Some(&COBALT_STAGES[..]));
    assert_eq!(rc.ticket_response.as_ref().unwrap().stages, ta.stages);

    // Recorded with its build.
    let mut row = None;
    for _ in 0..50 {
        row = sqlx::query_as::<_, (i16, Vec<i16>, String)>(
            "SELECT mode, stages, game FROM mm_matches WHERE match_id = $1",
        )
        .bind(&match_id)
        .fetch_optional(&stack.pool)
        .await
        .unwrap();
        if row.is_some() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let (mode, stages, game) = row.expect("match recorded");
    assert_eq!((mode, game.as_str()), (1, "cobalt-1.34"));
    assert_eq!(stages, COBALT_STAGES.iter().map(|s| *s as i16).collect::<Vec<_>>());

    // Its games are reported and in the history with the build, and move no Unranked rating.
    for by in [&u[0].1, &u[2].1] {
        let (st, body) = stack
            .post(
                "/v1/ranked/report-game",
                None,
                json!({
                    "uid": by.uid, "playKey": by.play_key, "matchId": match_id, "gameIndex": 1,
                    "winner": u[0].1.uid, "stageId": 74, "durationFrames": 9000,
                    "players": [{"uid": by.uid, "characterId": 0x3F, "stocksRemaining": 2, "damage": 40.5}],
                }),
            )
            .await;
        assert_eq!(st, 200, "{body}");
    }
    let rated: i64 =
        sqlx::query_scalar("SELECT count(*) FROM unranked_rated_games").fetch_one(&stack.pool).await.unwrap();
    let ratings: i64 =
        sqlx::query_scalar("SELECT count(*) FROM unranked_ratings").fetch_one(&stack.pool).await.unwrap();
    assert_eq!((rated, ratings), (0, 0));
    let (st, page) = stack.get("/v1/me/matches", Some(&u[0].0)).await;
    assert_eq!(st, 200, "{page}");
    let m = &page["matches"][0];
    assert_eq!(
        (m["matchId"].as_str(), m["mode"].as_str(), m["game"].as_str()),
        (Some(match_id.as_str()), Some("unranked"), Some("cobalt-1.34"))
    );

    // A build mm does not know, and Ranked on a mod: refused with what to do.
    let other = search(unranked(&stack, &u[1].1, "Cobalt:00000000000000000000000000000000", 3)).await;
    assert_eq!(other.status, Status::CreateError, "{other:?}");
    assert_eq!(other.error.as_deref(), Some("This version of Cobalt\ncan't play online here yet."));
    let mut ranked = unranked(&stack, &u[1].1, COBALT, 3);
    ranked.mode = 0;
    let r = search(ranked).await;
    assert_eq!(r.status, Status::CreateError, "{r:?}");
    assert_eq!(r.error.as_deref(), Some("Ranked isn't on Cobalt 1.34 yet."));
    stack.shutdown().await;
}

fn room_state(c: &mut OnlineClient) -> Value {
    c.wait_for(WAIT, |m| m["type"] == "room-state").expect("room-state")
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_room_takes_only_its_own_build() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["dana", "emil", "fred"]).await;
    let (mut a, hello) = OnlineClient::connect_as_game(stack.mm_addr, &creds(&u[0].1), "0.1.0", COBALT).unwrap();
    // (Cobalt's place for the stage select's turn line; a P+ game is told none.)
    assert_eq!(hello["type"], "hello-resp");
    assert_eq!(hello["gameAlive"], true);
    // Cobalt's own places: the turn line under the legend, Brawl's own cards.
    assert_eq!(hello["layout"]["sssLine"][0], 726.0);
    assert_eq!(hello["layout"]["cardPlates"].as_array().map(Vec::len), Some(3));
    assert!(hello["layout"].get("cssBar").is_none());
    let (mut b, hello) = OnlineClient::connect(stack.mm_addr, &creds(&u[1].1), "0.1.0").unwrap();
    assert_eq!(hello, json!({"type": "hello-resp", "gameAlive": true}));
    let (mut c, _) = OnlineClient::connect_as_game(stack.mm_addr, &creds(&u[2].1), "0.1.0", COBALT).unwrap();

    a.send(&RoomRequest::Create { public: true, ruleset: None, remembered: false });
    let code = room_state(&mut a)["code"].as_str().unwrap().to_string();
    // P+ can't join a Cobalt room; Cobalt can.
    b.send(&RoomRequest::Join { code: code.clone() });
    let err = b.wait_for(WAIT, |m| m["type"] == "room-error").expect("room-error");
    assert_eq!(err["error"], msg::ROOM_OTHER_GAME);
    c.send(&RoomRequest::Join { code: code.clone() });
    let s = a.wait_for(WAIT, |m| m["type"] == "room-state" && m["slots"][1]["player"].is_object()).expect("joined");
    assert_eq!(s["slots"][1]["player"]["connectCode"], u[2].1.connect_code);

    // The launcher's room list names the build of a mod's room.
    let deadline = std::time::Instant::now() + Duration::from_secs(8);
    loop {
        let (st, body) = stack.get("/v1/rooms", Some(&u[1].0)).await;
        assert_eq!(st, 200, "{body}");
        if let Some(r) = body["rooms"].as_array().and_then(|rs| rs.iter().find(|r| r["code"] == code.as_str())) {
            assert_eq!((r["game"].as_str(), r["gameName"].as_str()), (Some("cobalt-1.34"), Some("Cobalt 1.34")), "{r}");
            break;
        }
        assert!(std::time::Instant::now() < deadline, "the room never listed: {body}");
        tokio::time::sleep(Duration::from_millis(250)).await;
    }

    // A ruleset says which build its stage ids are of (`game`): the Cobalt room plays a Cobalt
    // ruleset, not a P+ one, and the P+ room the other way round.
    let weekly = |name: &str, game: Option<&str>| {
        let mut r = json!({
            "format": 1, "name": name, "rules": {"stocks": 3, "timeMinutes": 7},
            "stages": {"starters": [1, 46, 33, 25, 74], "counterpicks": [19, 2]},
            "bestOf": 3, "strikes": [1, 2, 1], "bans": 0, "dsr": false
        });
        if let Some(g) = game {
            r["game"] = json!(g);
        }
        json!({ "ruleset": r })
    };
    let (st, own) = stack.post("/v1/rulesets", Some(&u[0].0), weekly("Cobalt Weekly", Some("cobalt-1.34"))).await;
    assert_eq!((st, own["ruleset"]["game"].as_str()), (201, Some("cobalt-1.34")), "{own}");
    let (st, pplus) = stack.post("/v1/rulesets", Some(&u[0].0), weekly("P+ Weekly", None)).await;
    assert_eq!(st, 201, "{pplus}");
    assert!(pplus["ruleset"].get("game").is_none(), "{pplus}");
    let (own, pplus) = (own["code"].as_str().unwrap().to_string(), pplus["code"].as_str().unwrap().to_string());
    a.send(&RoomRequest::Ruleset { code: Some(pplus.clone()) });
    let err = a.wait_for(WAIT, |m| m["type"] == "room-error").expect("room-error");
    assert_eq!(err["error"], msg::RULESET_OTHER_GAME);
    a.send(&RoomRequest::Ruleset { code: Some(own.clone()) });
    let s = a.wait_for(WAIT, |m| m["type"] == "room-state" && m["ruleset"]["code"] == own.as_str()).expect("played");
    assert_eq!(s["ruleset"]["name"], "Cobalt Weekly");
    // It is now the account's remembered ruleset, for Cobalt's rooms: the same account's P+ game
    // creates a room as its own Create Room does and gets P+'s default, and its Cobalt game the
    // remembered one.
    a.close();
    let (mut a, _) = OnlineClient::connect(stack.mm_addr, &creds(&u[0].1), "0.1.0").unwrap();
    a.send(&RoomRequest::Create { public: false, ruleset: None, remembered: true });
    let s = room_state(&mut a);
    assert!(s["ruleset"]["code"].is_null(), "{s}");
    assert!(a.wait_for(Duration::from_millis(1500), |m| m["type"] == "room-state" && !m["ruleset"]["code"].is_null())
        .is_err());
    a.send(&RoomRequest::Ruleset { code: Some(own.clone()) });
    let err = a.wait_for(WAIT, |m| m["type"] == "room-error").expect("room-error");
    assert_eq!(err["error"], msg::RULESET_OTHER_GAME);
    a.close();
    let (mut a, _) = OnlineClient::connect_as_game(stack.mm_addr, &creds(&u[0].1), "0.1.0", COBALT).unwrap();
    a.send(&RoomRequest::Create { public: false, ruleset: None, remembered: true });
    let s = a.wait_for(WAIT, |m| m["type"] == "room-state" && m["ruleset"]["code"] == own.as_str()).expect("remembered");
    assert_eq!(s["ruleset"]["name"], "Cobalt Weekly");

    // A build mm does not know can't open the online connection at all.
    let (_, hello) =
        OnlineClient::connect_as_game(stack.mm_addr, &creds(&u[1].1), "0.1.0", "REX:812d2df3a26681ce57c335c54e675a8a")
            .unwrap();
    assert_eq!(hello["error"], "This version of REX\ncan't play online here yet.");
    a.close();
    b.close();
    c.close();
    stack.shutdown().await;
}
