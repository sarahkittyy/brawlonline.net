//! End-to-end: the launcher's social stream (`docs/social-protocol.md`). Postgres + accounts
//! (HTTP, server-sent events) + mm (for the inviter's room), with launchers made of reqwest and
//! the chat crypto from `common::chat`.

use std::time::{Duration, Instant};

use common::chat::{
    box_ad, encode_plaintext, identity_public, kx_public, open, pair_key, parse_plaintext, random_secret, seal,
    sign_kx, sign_message, verify_message,
};
use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mmclient::room::OnlineClient;
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(8);

/// A launcher: its keys and its open stream.
struct Launcher {
    uid: String,
    id_seed: [u8; 32],
    kx_secret: [u8; 32],
    resp: reqwest::Response,
    buf: String,
}

fn keys_for(uid: &str) -> ([u8; 32], [u8; 32], [(&'static str, String); 3]) {
    let id_seed = random_secret();
    let kx_secret = random_secret();
    let kx = kx_public(&kx_secret);
    let sig = sign_kx(&id_seed, &format!("dm-{uid}"), &kx);
    let headers =
        [("x-chat-key", hex(&identity_public(&id_seed))), ("x-chat-kx", hex(&kx)), ("x-chat-kx-sig", hex(&sig))];
    (id_seed, kx_secret, headers)
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

async fn connect(stack: &Stack, session: &str, user: &UserJson) -> Launcher {
    let (id_seed, kx_secret, headers) = keys_for(&user.uid);
    let mut req = stack.http.get(stack.url("/v1/social/stream")).bearer_auth(session);
    for (k, v) in headers {
        req = req.header(k, v);
    }
    let resp = req.send().await.expect("stream");
    assert_eq!(resp.status(), 200);
    assert_eq!(resp.headers()["content-type"], "text/event-stream");
    assert_eq!(resp.headers()["x-accel-buffering"], "no");
    Launcher { uid: user.uid.clone(), id_seed, kx_secret, resp, buf: String::new() }
}

impl Launcher {
    /// The next event (keep-alive comments skipped).
    async fn next(&mut self) -> Value {
        let deadline = Instant::now() + WAIT;
        loop {
            if let Some(end) = self.buf.find("\n\n") {
                let block: String = self.buf.drain(..end + 2).collect();
                let data: String = block.lines().filter_map(|l| l.strip_prefix("data:")).map(str::trim_start).collect();
                if !data.is_empty() {
                    return serde_json::from_str(&data).unwrap();
                }
                continue;
            }
            let left = deadline.saturating_duration_since(Instant::now());
            let chunk = tokio::time::timeout(left, self.resp.chunk()).await.expect("an event in time").unwrap();
            self.buf.push_str(std::str::from_utf8(&chunk.expect("stream still open")).unwrap());
        }
    }

    async fn next_of(&mut self, kind: &str) -> Value {
        loop {
            let ev = self.next().await;
            if ev["type"] == kind {
                return ev;
            }
        }
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn presence_dms_and_invites() {
    let stack = Stack::start(StackOptions { rate_limits: true, ..Default::default() }).await.unwrap();
    let (sa, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (sb, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (sc, carol) = stack.create_player("carol@example.test", "carol", "car").await;

    // Presence: alice sees nobody, then bob; bob sees alice with her keys.
    let mut a = connect(&stack, &sa, &alice).await;
    let w = a.next().await;
    assert_eq!(w["type"], "welcome");
    assert_eq!(w["you"], alice.uid.as_str());
    assert_eq!(w["online"], json!([]));
    assert_eq!(w["room"], Value::Null);
    let mut b = connect(&stack, &sb, &bob).await;
    let w = b.next().await;
    let listed = &w["online"][0];
    assert_eq!(listed["uid"], alice.uid.as_str());
    assert_eq!(listed["displayName"], "alice");
    assert_eq!(listed["connectCode"], alice.connect_code.as_str());
    let alice_kx = listed["kx"].as_str().unwrap().to_string();
    let joined = a.next_of("online").await;
    assert_eq!(joined["member"]["uid"], bob.uid.as_str());
    let bob_kx: [u8; 32] = unhex(joined["member"]["kx"].as_str().unwrap()).try_into().unwrap();

    // A DM, sealed by alice to bob's session key, opened by bob.
    let group = accounts::social::dm_group(&alice.uid, &bob.uid);
    let text = "hi bob ねこ".as_bytes();
    let sig = sign_message(&a.id_seed, &group, &a.uid, 1, text);
    let key = pair_key(&group, &a.kx_secret, &bob_kx).unwrap();
    let sealed = seal(&key, &[9u8; 24], &box_ad(&group, &a.uid, &b.uid), &encode_plaintext(1, &sig, text));
    let (st, body) = stack.post("/v1/social/dm", Some(&sa), json!({"to": bob.uid, "box": hex(&sealed)})).await;
    assert_eq!(st, 204, "{body}");
    let dm = b.next_of("dm").await;
    assert_eq!(dm["from"], alice.uid.as_str());
    assert_eq!(dm["kx"], alice_kx.as_str());
    let their_kx: [u8; 32] = unhex(&alice_kx).try_into().unwrap();
    let key_b = pair_key(&group, &b.kx_secret, &their_kx).unwrap();
    let plain = open(&key_b, &box_ad(&group, &alice.uid, &bob.uid), &unhex(dm["box"].as_str().unwrap())).unwrap();
    let (seq, sig, got) = parse_plaintext(&plain).unwrap();
    assert_eq!((seq, got), (1, text));
    assert!(verify_message(&identity_public(&a.id_seed), &group, &alice.uid, seq, got, &sig));

    // Refusals: not connected, offline, malformed, bad keys.
    let (st, body) = stack.post("/v1/social/dm", Some(&sc), json!({"to": bob.uid, "box": hex(&sealed)})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("not_connected")));
    let (st, body) = stack.post("/v1/social/dm", Some(&sa), json!({"to": carol.uid, "box": hex(&sealed)})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (404, Some("offline")));
    let (st, _) = stack.post("/v1/social/dm", Some(&sa), json!({"to": bob.uid, "box": "00ff"})).await;
    assert_eq!(st, 400);
    let (_, _, mut headers) = keys_for(&carol.uid);
    headers[2].1 = "00".repeat(64);
    let mut req = stack.http.get(stack.url("/v1/social/stream")).bearer_auth(&sc);
    for (k, v) in headers {
        req = req.header(k, v);
    }
    assert_eq!(req.send().await.unwrap().status(), 400, "a session key not signed by the identity key");

    // Outside a room an invite is to a Direct 1v1; accepting it tells the inviter bob's code.
    let (st, body) = stack.post("/v1/social/invite", Some(&sa), json!({"to": bob.uid})).await;
    assert_eq!((st, body["kind"].as_str()), (200, Some("direct")));
    let inv = b.next_of("invite").await;
    assert_eq!(inv["kind"], "direct");
    assert_eq!(inv["connectCode"], alice.connect_code.as_str());
    let (st, _) = stack.post("/v1/social/invite/answer", Some(&sb), json!({"from": alice.uid, "accept": true})).await;
    assert_eq!(st, 204);
    let ans = a.next_of("invite-answer").await;
    assert_eq!((ans["accept"].as_bool(), ans["connectCode"].as_str()), (Some(true), Some(bob.connect_code.as_str())));
    // One invite to the same player per 5 s.
    tokio::time::sleep(Duration::from_secs(5)).await;

    // In a room, the invite is to the room (it needs an open slot).
    let (mut game, _) = OnlineClient::connect(stack.mm_addr, &creds(&alice), "0.1.0").expect("connect");
    game.send(&RoomRequest::Create { public: false, ruleset: None, remembered: false });
    let state = game.wait_for(WAIT, |m| m["type"] == "room-state").expect("room-state");
    let code = state["code"].as_str().unwrap().to_string();
    let room = a.next_of("room").await;
    assert_eq!(room["room"]["code"], code.as_str());
    assert_eq!(room["room"]["joinable"], true);
    let (st, body) = stack.post("/v1/social/invite", Some(&sa), json!({"to": bob.uid})).await;
    assert_eq!((st, body["kind"].as_str()), (200, Some("room")), "{body}");
    let inv = b.next_of("invite").await;
    assert_eq!(inv["from"], alice.uid.as_str());
    assert_eq!(inv["kind"], "room");
    assert_eq!(inv["room"]["code"], code.as_str());
    let (st, _) = stack.post("/v1/social/invite", Some(&sa), json!({"to": bob.uid})).await;
    assert_eq!(st, 429, "one invite to the same player per 5 s");

    // The private room stays private: /v1/rooms lists neither it nor where anyone is.
    let (st, list) = stack.get("/v1/rooms", Some(&sa)).await;
    assert_eq!(st, 200);
    assert_eq!(list["rooms"], json!([]));
    assert!(list.get("members").is_none(), "{list}");

    // Leaving the room reaches alice's launcher; closing her launcher reaches bob's.
    game.send(&RoomRequest::Leave);
    assert_eq!(a.next_of("room").await["room"], Value::Null);
    drop(a);
    assert_eq!(b.next_of("offline").await["uid"], alice.uid.as_str());

    // A second launcher on bob's account replaces the first.
    let mut b2 = connect(&stack, &sb, &bob).await;
    assert_eq!(b2.next().await["type"], "welcome");
    assert_eq!(b.next_of("replaced").await["type"], "replaced");

    drop(game);
    stack.shutdown().await;
}
