//! End-to-end: spectating (`docs/spectate-protocol.md`). Postgres + accounts (HTTP) + mm
//! (ENet/UDP): two players make a Direct match; one game offers and uploads its replay's chunks
//! over the online connection like Dolphin does; a third account lists the game, watches it over
//! server-sent events and chats; the players can't watch their own game.

use std::time::{Duration, Instant};

use common::spectate::encode_chunk;
use e2e::{creds, Stack, StackOptions, UserJson};
use mm::engine::EngineConfig;
use mm::live::{LiveConfig, SpectateMode};
use mmclient::room::OnlineClient;
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(5);

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

fn online(stack: &Stack, user: &UserJson) -> OnlineClient {
    let (client, hello) = OnlineClient::connect(stack.mm_addr, &creds(user), "0.1.0").expect("connect");
    assert_eq!(hello["type"], "hello-resp");
    client
}

fn frms(first: u32, count: u16) -> Vec<u8> {
    let mut raw = Vec::new();
    raw.extend_from_slice(&first.to_le_bytes());
    raw.extend_from_slice(&count.to_le_bytes());
    raw.push(0b11);
    raw.push(0);
    raw.extend(std::iter::repeat_n(0x11u8, 2 * count as usize * 0x40));
    raw.extend(std::iter::repeat_n(0x22u8, count as usize * 4));
    raw.extend_from_slice(&0u16.to_le_bytes());
    raw
}

/// A watch stream's events, read as they come.
struct Events {
    resp: reqwest::Response,
    buf: String,
}

impl Events {
    async fn next(&mut self) -> Value {
        let deadline = Instant::now() + WAIT;
        loop {
            if let Some(end) = self.buf.find("\n\n") {
                let event: String = self.buf.drain(..end + 2).collect();
                let data: String =
                    event.lines().filter_map(|l| l.strip_prefix("data:")).map(|d| d.trim_start()).collect();
                if data.is_empty() {
                    continue; // a ping
                }
                return serde_json::from_str(&data).unwrap();
            }
            let left = deadline.saturating_duration_since(Instant::now());
            let chunk = tokio::time::timeout(left, self.resp.chunk()).await.expect("event in time").unwrap();
            self.buf.push_str(std::str::from_utf8(&chunk.expect("stream open")).unwrap());
        }
    }

    async fn until(&mut self, kind: &str) -> Value {
        loop {
            let e = self.next().await;
            if e["type"] == kind {
                return e;
            }
        }
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_direct_game_is_listed_watched_and_chatted_about() {
    let engine =
        EngineConfig { live: LiveConfig { mode: SpectateMode::On, ..Default::default() }, ..Default::default() };
    let stack = Stack::start(StackOptions { engine, ..Default::default() }).await.unwrap();
    let (alice_token, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (carol_token, _) = stack.create_player("carol@example.test", "carol", "car").await;
    let (mut a, mut b) = (online(&stack, &alice), online(&stack, &bob));

    let oa = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let ob = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    let (ra, rb) = tokio::join!(search(oa), search(ob));
    assert_eq!((ra.status, rb.status), (Status::Matched, Status::Matched));
    let match_id = ra.ticket_response.as_ref().unwrap().match_id.clone().unwrap();

    // Only alice lets others stream her games; bob's game sends nothing. After 2 s alice uploads.
    let game_uid = "ab".repeat(16);
    a.send_json(&json!({"type": "spectate-offer", "matchId": match_id, "gameUid": game_uid, "game": 1}));
    let role = a.wait_for(WAIT, |m| m["type"] == "spectate-role").unwrap();
    assert_eq!((role["role"].as_str(), role["headers"].as_bool()), (Some("primary"), Some(true)));
    let slot = role["slot"].as_u64().unwrap() as u32;
    let info = json!({
        "matchId": match_id, "gameIndex": 1, "stageId": 7, "isTeams": false,
        "players": [
            {"port": 1, "uid": alice.uid, "displayName": "not alice", "connectCode": "X#1", "characterId": 17, "costume": 2},
            {"port": 2, "uid": bob.uid, "displayName": "bob", "connectCode": bob.connect_code, "characterId": 9, "costume": 0}
        ]
    });
    a.upload_chunk(slot, 0, &encode_chunk(b"INFO", info.to_string().as_bytes(), false));
    a.upload_chunk(slot, 1, &encode_chunk(b"SETP", b"{\"setup\":1}", true));
    a.upload_chunk(slot, 2, &encode_chunk(b"FRMS", &frms(120, 60), true));

    // Listed for carol, with mm's names (not INFO's) and INFO's characters.
    let mut games = Value::Null;
    let deadline = Instant::now() + WAIT;
    while Instant::now() < deadline {
        a.pump();
        b.pump();
        let (st, body) = stack.get("/v1/live", Some(&carol_token)).await;
        assert_eq!(st, 200, "{body}");
        if !body["games"].as_array().unwrap().is_empty() {
            games = body["games"].clone();
            break;
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
    }
    let g = &games[0];
    assert_eq!(
        (g["mode"].as_str(), g["game"]["number"].as_u64(), g["game"]["stageId"].as_u64()),
        (Some("direct"), Some(1), Some(7))
    );
    assert_eq!(g["players"][0]["displayName"], "alice");
    assert_eq!(g["players"][0]["characterId"], 17);
    assert_eq!(g["spectators"], 0);
    let id = g["id"].as_str().unwrap().to_string();

    // A player may not watch their own game.
    let (st, body) = stack.get(&format!("/v1/live/{id}/watch"), Some(&alice_token)).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("playing")));

    // Carol watches: welcome, the game, its three chunks so far.
    let resp =
        stack.http.get(stack.url(&format!("/v1/live/{id}/watch"))).bearer_auth(&carol_token).send().await.unwrap();
    assert_eq!(resp.status(), 200);
    let mut ev = Events { resp, buf: String::new() };
    let welcome = ev.next().await;
    assert_eq!((welcome["type"].as_str(), welcome["stream"]["spectators"].as_u64()), (Some("welcome"), Some(1)));
    let game = ev.next().await;
    assert_eq!(
        (game["type"].as_str(), game["game"].as_u64(), game["uid"].as_str()),
        (Some("game"), Some(1), Some(game_uid.as_str()))
    );
    for i in 0..3 {
        let c = ev.next().await;
        assert_eq!((c["type"].as_str(), c["i"].as_u64()), (Some("chunk"), Some(i)));
    }

    // A new chunk comes as it is uploaded.
    a.upload_chunk(slot, 3, &encode_chunk(b"FRMS", &frms(180, 60), true));
    let c = ev.until("chunk").await;
    assert_eq!(c["i"], 3);

    // Carol chats; she hears it, with her account's name.
    let (st, body) = stack.post(&format!("/v1/live/{id}/chat"), Some(&carol_token), json!({"text": "nice"})).await;
    assert_eq!(st, 204, "{body}");
    let line = ev.until("chat").await;
    assert_eq!((line["line"]["text"].as_str(), line["line"]["displayName"].as_str()), (Some("nice"), Some("carol")));
    // Alice isn't watching: she can't chat there.
    let (st, body) = stack.post(&format!("/v1/live/{id}/chat"), Some(&alice_token), json!({"text": "hi"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_watching")));
    // Nothing of the spectator chat ever reaches the players' games.
    for m in a.drain(Duration::from_millis(300)).into_iter().chain(b.drain(Duration::from_millis(300))) {
        assert!(!m.to_string().contains("nice"), "a player's game got spectator chat: {m}");
    }

    // The game ends: carol gets `end`.
    let endg = json!({"endMethod": "game", "winnerPort": 1});
    a.upload_chunk(slot, 4, &encode_chunk(b"ENDG", endg.to_string().as_bytes(), false));
    let end = ev.until("end").await;
    assert_eq!(end["game"], 1);
    // The list comes from a snapshot about a second old.
    let deadline = Instant::now() + WAIT;
    let body = loop {
        let (_, body) = stack.get("/v1/live", Some(&carol_token)).await;
        if body["games"][0]["game"]["state"] == "between" || Instant::now() > deadline {
            break body;
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
    };
    assert_eq!(body["games"][0]["game"]["state"], "between");
    assert_eq!(body["games"][0]["players"][0]["wins"], 1);
    assert_eq!(body["games"][0]["spectators"], 1);

    a.close();
    b.close();
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn off_by_default_nothing_is_uploaded_or_listed() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (carol_token, _) = stack.create_player("carol@example.test", "carol", "car").await;
    let mut a = online(&stack, &alice);
    let oa = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let ob = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    let (ra, _) = tokio::join!(search(oa), search(ob));
    let match_id = ra.ticket_response.as_ref().unwrap().match_id.clone().unwrap();
    a.send_json(&json!({"type": "spectate-offer", "matchId": match_id, "gameUid": "cd".repeat(16), "game": 1}));
    let role = a.wait_for(WAIT, |m| m["type"] == "spectate-role").unwrap();
    assert_eq!(role["role"], "none");
    let (st, body) = stack.get("/v1/live", Some(&carol_token)).await;
    assert_eq!((st, body["games"].as_array().map(|g| g.len())), (200, Some(0)));
    a.close();
    stack.shutdown().await;
}
