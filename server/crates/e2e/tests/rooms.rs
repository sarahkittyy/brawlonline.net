//! End-to-end: rooms (`docs/rooms-protocol.md`). Postgres + accounts (HTTP) + mm (ENet/UDP),
//! accounts made over HTTP, and fake games that keep an online connection to mm like a running
//! game does (`mmclient::room`).

use std::time::{Duration, Instant};

use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mm::engine::EngineConfig;
use mm::messages as msg;
use mmclient::room::OnlineClient;
use mmclient::Status;
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(5);

/// A game that is running and logged in.
fn online(stack: &Stack, user: &UserJson) -> OnlineClient {
    let (client, hello) = OnlineClient::connect(stack.mm_addr, &creds(user), "0.1.0").expect("connect");
    assert_eq!(hello, json!({"type": "hello-resp"}));
    client
}

fn room_state(c: &mut OnlineClient) -> Value {
    c.wait_for(WAIT, |m| m["type"] == "room-state").expect("room-state")
}

/// The room-state that `pred` accepts (earlier ones are skipped).
fn room_state_where(c: &mut OnlineClient, pred: impl Fn(&Value) -> bool) -> Value {
    c.wait_for(WAIT, |m| m["type"] == "room-state" && pred(m)).expect("room-state")
}

fn room_error(c: &mut OnlineClient) -> Value {
    c.wait_for(WAIT, |m| m["type"] == "room-error").expect("room-error")
}

fn create(c: &mut OnlineClient) -> String {
    c.send(&RoomRequest::Create { public: true });
    room_state(c)["code"].as_str().unwrap().to_string()
}

fn join(c: &mut OnlineClient, code: &str) {
    c.send(&RoomRequest::Join { code: code.into() });
}

/// `GET /v1/rooms` until `pred` holds (mm publishes twice a second, accounts caches for 1 s).
async fn rooms_until(stack: &Stack, session: &str, pred: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        let (st, body) = stack.get("/v1/rooms", Some(session)).await;
        assert_eq!(st, 200, "{body}");
        if pred(&body) {
            return body;
        }
        assert!(Instant::now() < deadline, "the room list never matched: {body}");
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn create_join_full_not_found_kick_and_host_hand_over() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (_, carol) = stack.create_player("carol@example.test", "carol", "car").await;
    let (_, dave) = stack.create_player("dave@example.test", "dave", "dav").await;
    let (mut a, mut b, mut c, mut d) =
        (online(&stack, &alice), online(&stack, &bob), online(&stack, &carol), online(&stack, &dave));

    let code = create(&mut a);
    assert_eq!(code.len(), 4);
    assert!(code.bytes().all(|x| b"BCDFGHJKLMNPQRSTVWXZ".contains(&x)), "{code}");

    // Bob joins by code (typed in lower case); both see both.
    join(&mut b, &code.to_lowercase());
    let sb = room_state(&mut b);
    assert_eq!((sb["you"].as_u64(), sb["host"].as_u64()), (Some(2), Some(1)));
    let sa = room_state_where(&mut a, |s| !s["slots"][1]["player"].is_null());
    assert_eq!(sa["slots"][1]["player"]["displayName"], "bob");
    assert_eq!(sa["slots"][1]["player"]["connectCode"], bob.connect_code.as_str());
    assert_eq!(sa["slots"][0]["host"], true);
    assert_eq!(sa["slots"][0]["player"]["ready"], false);

    // Slots 3-4 are closed: carol does not fit. A code that is no room is not found.
    join(&mut c, &code);
    assert_eq!(room_error(&mut c)["error"], msg::ROOM_FULL);
    let other = if code == "BBBB" { "CCCC" } else { "BBBB" };
    join(&mut d, other);
    assert_eq!(room_error(&mut d)["error"], msg::ROOM_NOT_FOUND);
    join(&mut d, "A#1");
    assert_eq!(room_error(&mut d)["error"], msg::ROOM_NOT_FOUND);

    // Only the host changes slots; then carol fits.
    b.send(&RoomRequest::Slot { slot: 3, open: true });
    assert_eq!(room_error(&mut b)["error"], msg::HOST_ONLY);
    a.send(&RoomRequest::Slot { slot: 3, open: true });
    room_state_where(&mut a, |s| s["slots"][2]["open"] == true);
    join(&mut c, &code);
    assert_eq!(room_state(&mut c)["you"], 3);

    // Closing bob's slot removes him; he cannot come back.
    a.send(&RoomRequest::Slot { slot: 2, open: false });
    let left = b.wait_for(WAIT, |m| m["type"] == "room-left").unwrap();
    assert_eq!(left, json!({"type": "room-left", "code": code, "reason": msg::REMOVED}));
    let sc = room_state_where(&mut c, |s| s["slots"][1]["player"].is_null());
    assert_eq!(sc["slots"][1]["open"], false);
    a.send(&RoomRequest::Slot { slot: 2, open: true });
    room_state_where(&mut a, |s| s["slots"][1]["open"] == true);
    join(&mut b, &code);
    assert_eq!(room_error(&mut b)["error"], msg::REMOVED);

    // The host's game closes: carol (the earliest-joined left) is the host, the code stays.
    a.close();
    let sc = room_state_where(&mut c, |s| s["host"] == 3);
    assert_eq!(sc["code"], code.as_str());
    assert!(sc["slots"][0]["player"].is_null());
    join(&mut d, &code);
    let sd = room_state(&mut d);
    assert_eq!((sd["you"].as_u64(), sd["host"].as_u64()), (Some(1), Some(3)));

    // Everyone leaves: the room is gone and its code with it.
    c.send(&RoomRequest::Leave);
    c.wait_for(WAIT, |m| m["type"] == "room-left").unwrap();
    d.send(&RoomRequest::Leave);
    d.wait_for(WAIT, |m| m["type"] == "room-left").unwrap();
    join(&mut b, &code);
    assert_eq!(room_error(&mut b)["error"], msg::ROOM_NOT_FOUND);
    for x in [b, c, d] {
        x.close();
    }
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn public_list_private_rooms_and_online_count() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (sa, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (_, carol) = stack.create_player("carol@example.test", "carol", "car").await;

    // The list names players: logged-in launchers only.
    let (st, body) = stack.get("/v1/rooms", None).await;
    assert_eq!(st, 401, "{body}");
    let body = rooms_until(&stack, &sa, |b| b["online"] == 0).await;
    assert_eq!(body["rooms"], json!([]));
    assert!(body["updatedAt"].is_string());

    let mut a = online(&stack, &alice);
    let mut b = online(&stack, &bob);
    let c = online(&stack, &carol);
    let public = create(&mut a);
    b.send(&RoomRequest::Create { public: false });
    let private = room_state(&mut b)["code"].as_str().unwrap().to_string();
    // The private room shows as private to its members.
    b.send(&RoomRequest::Public { public: false });
    assert_eq!(room_state(&mut b)["public"], false);

    let body =
        rooms_until(&stack, &sa, |b| b["online"] == 3 && b["rooms"].as_array().is_some_and(|r| r.len() == 1)).await;
    assert_eq!(
        body["rooms"][0],
        json!({
            "code": public,
            "host": "alice",
            "players": 1,
            "openSlots": 2,
            "mode": "1v1",
            "status": "waiting",
            "names": ["alice"],
            "joinable": true,
        })
    );
    // A private room is joinable by code.
    let mut c = c;
    join(&mut c, &private);
    assert_eq!(room_state(&mut c)["public"], false);

    // The host makes it public: it is listed (full, so not joinable, and after the joinable one).
    b.send(&RoomRequest::Public { public: true });
    let body = rooms_until(&stack, &sa, |b| b["rooms"].as_array().is_some_and(|r| r.len() == 2)).await;
    assert_eq!(body["rooms"][0]["code"], public.as_str());
    assert_eq!(body["rooms"][1]["code"], private.as_str());
    assert_eq!(body["rooms"][1]["joinable"], false);
    assert_eq!(body["rooms"][1]["names"], json!(["bob", "carol"]));

    // Teams with four open slots: mode "teams"; three open without Teams: "ffa".
    a.send(&RoomRequest::Slot { slot: 3, open: true });
    a.send(&RoomRequest::Slot { slot: 4, open: true });
    a.send(&RoomRequest::Teams { on: true });
    rooms_until(&stack, &sa, |b| b["rooms"][0]["mode"] == "teams" && b["rooms"][0]["openSlots"] == 4).await;
    a.send(&RoomRequest::Teams { on: false });
    a.send(&RoomRequest::Slot { slot: 4, open: false });
    rooms_until(&stack, &sa, |b| b["rooms"][0]["mode"] == "ffa" && b["rooms"][0]["openSlots"] == 3).await;

    // Games that close are no longer online; their rooms go with them.
    c.close();
    b.close();
    let body = rooms_until(&stack, &sa, |b| b["online"] == 1).await;
    assert_eq!(body["rooms"].as_array().unwrap().len(), 1);
    a.close();
    rooms_until(&stack, &sa, |b| b["online"] == 0 && b["rooms"] == json!([])).await;

    // mm gone: the launcher gets a clear 503.
    let mut stack = stack;
    if let Some(m) = stack.mm.take() {
        tokio::task::spawn_blocking(move || m.stop()).await.unwrap();
    }
    tokio::time::sleep(Duration::from_millis(1200)).await;
    let (st, body) = stack.get("/v1/rooms", Some(&sa)).await;
    assert_eq!(st, 503, "{body}");
    assert_eq!(body["error"]["code"], "mm_unavailable");
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_room_plays_with_every_member_and_takes_a_joiner_during_the_game() {
    // Rooms play with Direct's ruleset (its stage list goes with the game).
    let engine = EngineConfig { rulesets: mm::ruleset::Rulesets::load(None).unwrap(), ..Default::default() };
    let stack = Stack::start(StackOptions { engine, ..Default::default() }).await.unwrap();
    let (sa, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (_, carol) = stack.create_player("carol@example.test", "carol", "car").await;
    let (_, dave) = stack.create_player("dave@example.test", "dave", "dav").await;
    let (mut a, mut b, mut c) = (online(&stack, &alice), online(&stack, &bob), online(&stack, &carol));

    let code = create(&mut a);
    a.send(&RoomRequest::Slot { slot: 3, open: true });
    a.send(&RoomRequest::Teams { on: true });
    join(&mut b, &code);
    join(&mut c, &code);
    room_state_where(&mut c, |s| s["you"] == 3);
    // Everyone on one colour: no start.
    for x in [&mut a, &mut b, &mut c] {
        x.send(&RoomRequest::Team { team: 1 });
        x.send(&RoomRequest::Ready { ready: true, character: Some(7), costume: Some(1) });
    }
    let s = room_state_where(&mut a, |s| {
        s["slots"].as_array().unwrap().iter().filter(|x| x["player"]["ready"] == true).count() == 3
    });
    assert_eq!(s["statusText"], msg::PICK_TEAMS);
    assert_eq!(s["mode"], "teams");
    assert_eq!(s["slots"][1]["player"]["character"], 7);
    // 2v1: the room starts by itself.
    c.send(&RoomRequest::Team { team: 0 });
    let mut match_ids = vec![];
    for x in [&mut a, &mut b, &mut c] {
        let start = x.wait_for(WAIT, |m| m["type"] == "room-start").unwrap();
        assert_eq!(start["code"], code.as_str());
        match_ids.push(start["matchId"].as_str().unwrap().to_string());
    }
    assert!(match_ids.iter().all(|m| m == &match_ids[0] && m.starts_with(&format!("mode.room-{code}-"))));

    // Each game sends its room ticket from its P2P port; all three get one response listing all.
    let tickets: Vec<_> = [&alice, &bob, &carol]
        .into_iter()
        .map(|u| {
            let (server, cr, code) = (stack.mm_addr, creds(u), code.clone());
            tokio::task::spawn_blocking(move || mmclient::room::room_ticket(server, cr, &code))
        })
        .collect();
    let mut results = vec![];
    for t in tickets {
        results.push(t.await.unwrap().unwrap());
    }
    for (i, r) in results.iter().enumerate() {
        assert_eq!(r.status, Status::Matched, "{r:?}");
        let resp = r.ticket_response.as_ref().unwrap();
        assert_eq!(resp.match_id.as_deref(), Some(match_ids[0].as_str()));
        assert_eq!(resp.is_host, Some(i == 0), "the room's host decides");
        let players = resp.players.as_ref().unwrap();
        assert_eq!(players.iter().map(|p| p.port).collect::<Vec<_>>(), vec![1, 2, 3]);
        let local = players.iter().find(|p| p.is_local_player).unwrap();
        assert_eq!(local.port as usize, i + 1);
        assert_eq!(local.ip_address, format!("127.0.0.1:{}", r.local_port));
        assert_eq!(r.remote_addresses.len(), 2);
        assert_eq!(resp.stages.as_ref().unwrap().len(), 15);
    }
    let s = room_state_where(&mut a, |s| s["status"] == "in-game");
    assert_eq!(s["slots"][0]["player"]["inGame"], true);
    assert_eq!(s["slots"][0]["player"]["ready"], false);
    let body = rooms_until(&stack, &sa, |b| b["rooms"][0]["status"] == "in-game").await;
    assert_eq!(body["rooms"][0]["joinable"], false);

    // Carol's game closes in the middle of the match: her slot is open and empty, the room is
    // joinable while the others play, and dave waits for the game to end.
    c.close();
    rooms_until(&stack, &sa, |b| b["rooms"][0]["joinable"] == true && b["rooms"][0]["players"] == 2).await;
    let mut d = online(&stack, &dave);
    join(&mut d, &code);
    let sd = room_state(&mut d);
    assert_eq!(sd["you"], 3);
    assert_eq!(sd["statusText"], msg::WAITING_FOR_GAME);
    // Slots wait for the game to end.
    a.send(&RoomRequest::Slot { slot: 3, open: false });
    assert_eq!(room_error(&mut a)["error"], msg::BETWEEN_GAMES);

    // Both players come back: the room waits again, nobody is ready.
    a.send(&RoomRequest::Back);
    b.send(&RoomRequest::Back);
    let s = room_state_where(&mut d, |s| s["status"] == "waiting");
    assert_eq!(s["statusText"], msg::waiting_on(&["alice", "bob", "dave"]));
    for x in [a, b, d] {
        x.close();
    }
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn each_platform_needs_its_newest_build_from_the_update_feed() {
    let feed = std::env::temp_dir().join(format!(
        "e2e-update-feed-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
    ));
    std::fs::create_dir_all(&feed).unwrap();
    // pp-release's folder: Windows has 0.2.0, the macOS build of it still waits for Apple.
    std::fs::write(feed.join("latest.yml"), "version: 0.2.0\npath: Brawl-Online-Setup-0.2.0.exe\n").unwrap();
    std::fs::write(feed.join("latest-mac.yml"), "version: 0.1.0\n").unwrap();
    let stack = Stack::start(StackOptions {
        engine: EngineConfig { update_feed_dir: Some(feed.clone()), ..Default::default() },
        ..Default::default()
    })
    .await
    .unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bobb").await;
    // mm reads the feed on its first tick.
    tokio::time::sleep(Duration::from_millis(300)).await;
    let (_, hello) = OnlineClient::connect_on(stack.mm_addr, &creds(&alice), "0.1.0", "win").unwrap();
    assert_eq!(hello, json!({"type": "hello-resp", "error": msg::update_to("0.2.0"), "latestVersion": "0.2.0"}));
    let (mut a, hello) = OnlineClient::connect_on(stack.mm_addr, &creds(&alice), "0.2.0", "win").unwrap();
    assert_eq!(hello, json!({"type": "hello-resp"}));
    let (mut b, hello) = OnlineClient::connect_on(stack.mm_addr, &creds(&bob), "0.1.0", "mac").unwrap();
    assert_eq!(hello, json!({"type": "hello-resp"}));
    let code = create(&mut a);
    join(&mut b, &code);
    room_state_where(&mut a, |s| s["slots"][1]["player"].is_object());

    // The macOS build is published while bob's game runs 0.1.0: his START is refused.
    std::fs::write(feed.join("latest-mac.yml"), "version: 0.2.0\n").unwrap();
    // mm reads the feed again within 10 s; both games keep their connections serviced meanwhile.
    let until = Instant::now() + Duration::from_millis(10_500);
    while Instant::now() < until {
        a.pump();
        b.pump();
        std::thread::sleep(Duration::from_millis(20));
    }
    b.send(&RoomRequest::Ready { ready: true, character: Some(7), costume: Some(0) });
    let e = room_error(&mut b);
    assert_eq!(e, json!({"type": "room-error", "op": "room-ready", "error": msg::update_to("0.2.0")}));
    a.send(&RoomRequest::Ready { ready: true, character: Some(7), costume: Some(0) });
    let s = room_state_where(&mut a, |s| s["slots"][0]["player"]["ready"] == true);
    assert_eq!(s["slots"][1]["player"]["ready"], false, "{s}");
    for x in [a, b] {
        x.close();
    }
    stack.shutdown().await;
    let _ = std::fs::remove_dir_all(&feed);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn hello_needs_a_current_game_and_a_valid_login() {
    let stack = Stack::start(StackOptions {
        engine: EngineConfig {
            min_app_version: Some("0.2.0".into()),
            latest_version: Some("0.2.1".into()),
            ..Default::default()
        },
        ..Default::default()
    })
    .await
    .unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, hello) = OnlineClient::connect(stack.mm_addr, &creds(&alice), "0.1.0").unwrap();
    assert_eq!(hello["error"], msg::update_to("0.2.1"));
    assert_eq!(hello["latestVersion"], "0.2.1");
    let mut forged = creds(&alice);
    forged.play_key = "0".repeat(64);
    let (_, hello) = OnlineClient::connect(stack.mm_addr, &forged, "0.2.0").unwrap();
    assert_eq!(hello["error"], msg::LOGIN_EXPIRED);
    let (mut a, hello) = OnlineClient::connect(stack.mm_addr, &creds(&alice), "0.2.0").unwrap();
    assert_eq!(hello, json!({"type": "hello-resp"}));
    // Bad room requests are answered; the connection stays.
    a.send_json(&json!({"type": "room-slot", "slot": 9, "open": true}));
    assert_eq!(room_error(&mut a)["error"], msg::INVALID_REQUEST);
    a.send(&RoomRequest::Leave);
    assert_eq!(room_error(&mut a)["error"], msg::NOT_IN_ROOM);
    assert!(a.is_connected());
    a.close();
    stack.shutdown().await;
}
