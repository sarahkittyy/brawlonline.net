//! End-to-end: friends, friend requests, the ignore list on the account and friends-only rooms
//! (`docs/design/friends.md`). Postgres + accounts (HTTP, the social stream) + mm (rooms).

use std::time::{Duration, Instant};

use common::chat::{identity_public, kx_public, random_secret, sign_kx};
use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mmclient::room::OnlineClient;
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(8);

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// A launcher's social stream.
struct Launcher {
    resp: reqwest::Response,
    buf: String,
}

async fn connect(stack: &Stack, session: &str, user: &UserJson) -> Launcher {
    let id_seed = random_secret();
    let kx_secret = random_secret();
    let kx = kx_public(&kx_secret);
    let sig = sign_kx(&id_seed, &format!("dm-{}", user.uid), &kx);
    let resp = stack
        .http
        .get(stack.url("/v1/social/stream"))
        .bearer_auth(session)
        .header("x-chat-key", hex(&identity_public(&id_seed)))
        .header("x-chat-kx", hex(&kx))
        .header("x-chat-kx-sig", hex(&sig))
        .send()
        .await
        .expect("stream");
    assert_eq!(resp.status(), 200);
    let mut l = Launcher { resp, buf: String::new() };
    assert_eq!(l.next(WAIT).await.expect("welcome")["type"], "welcome");
    l
}

impl Launcher {
    /// The next event within `wait` (keep-alive comments skipped).
    async fn next(&mut self, wait: Duration) -> Option<Value> {
        let deadline = Instant::now() + wait;
        loop {
            if let Some(end) = self.buf.find("\n\n") {
                let block: String = self.buf.drain(..end + 2).collect();
                let data: String = block.lines().filter_map(|l| l.strip_prefix("data:")).map(str::trim_start).collect();
                if !data.is_empty() {
                    return Some(serde_json::from_str(&data).unwrap());
                }
                continue;
            }
            let left = deadline.saturating_duration_since(Instant::now());
            let chunk = tokio::time::timeout(left, self.resp.chunk()).await.ok()?.unwrap();
            self.buf.push_str(std::str::from_utf8(&chunk.expect("stream still open")).unwrap());
        }
    }

    async fn friends_event(&mut self) -> Value {
        loop {
            let ev = self.next(WAIT).await.expect("a friends event");
            if ev["type"] == "friends" {
                return ev;
            }
        }
    }

    /// No `friends` event for a moment.
    async fn no_friends_event(&mut self) {
        while let Some(ev) = self.next(Duration::from_millis(700)).await {
            assert_ne!(ev["type"], "friends", "{ev}");
        }
    }
}

fn uids(list: &Value) -> Vec<String> {
    list.as_array().unwrap().iter().map(|p| p["uid"].as_str().unwrap().to_string()).collect()
}

async fn friends_of(stack: &Stack, session: &str) -> Value {
    let (st, v) = stack.get("/v1/friends", Some(session)).await;
    assert_eq!(st, 200, "{v}");
    v
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

fn room_codes(body: &Value) -> Vec<String> {
    body["rooms"].as_array().unwrap().iter().map(|r| r["code"].as_str().unwrap().to_string()).collect()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn requests_ignores_and_friends_only_rooms() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (sa, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (sb, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (sc, carol) = stack.create_player("carol@example.test", "carol", "car").await;
    let (sd, dave) = stack.create_player("dave@example.test", "dave", "dav").await;
    let mut b = connect(&stack, &sb, &bob).await;
    let mut a = connect(&stack, &sa, &alice).await;

    // Refusals: nobody with that code, yourself.
    let (st, v) = stack.post("/v1/friends/requests", Some(&sa), json!({"to": "ZZZZ#1"})).await;
    assert_eq!((st, v["error"]["code"].as_str()), (404, Some("no_player")), "{v}");
    assert_eq!(v["error"]["message"], "No player has the code ZZZZ#1.");
    let (st, _) = stack.post("/v1/friends/requests", Some(&sa), json!({"to": alice.connect_code})).await;
    assert_eq!(st, 400);

    // Alice asks bob by his code (any case); bob's launcher is told, with her name.
    let (st, v) = stack.post("/v1/friends/requests", Some(&sa), json!({"to": bob.connect_code.to_lowercase()})).await;
    assert_eq!((st, v["status"].as_str()), (200, Some("sent")), "{v}");
    assert_eq!(v["player"]["displayName"], "bob");
    assert_eq!(uids(&v["outgoing"]), vec![bob.uid.clone()]);
    let ev = b.friends_event().await;
    assert_eq!((ev["change"].as_str(), ev["player"]["uid"].as_str()), (Some("request"), Some(alice.uid.as_str())));
    assert_eq!(ev["player"]["connectCode"], alice.connect_code.as_str());
    assert_eq!(uids(&friends_of(&stack, &sb).await["incoming"]), vec![alice.uid.clone()]);

    // Bob declines: gone from his list, and nobody tells alice (she still sees it waiting).
    let (st, v) = stack.post(&format!("/v1/friends/requests/{}/decline", alice.uid), Some(&sb), json!({})).await;
    assert_eq!(st, 200, "{v}");
    assert!(uids(&v["incoming"]).is_empty());
    a.no_friends_event().await;
    assert_eq!(uids(&friends_of(&stack, &sa).await["outgoing"]), vec![bob.uid.clone()]);
    // Asking again doesn't ask him again.
    let (_, v) = stack.post("/v1/friends/requests", Some(&sa), json!({"to": bob.connect_code})).await;
    assert_eq!(v["status"], "sent");
    b.no_friends_event().await;
    assert!(uids(&friends_of(&stack, &sb).await["incoming"]).is_empty());

    // Bob asks alice (by her profile link): she had asked, so they are friends at once.
    let link = format!("https://brawlonline.net/profile/{}", alice.connect_code.replace('#', "-").to_lowercase());
    let (st, v) = stack.post("/v1/friends/requests", Some(&sb), json!({"to": link})).await;
    assert_eq!((st, v["status"].as_str()), (200, Some("friends")), "{v}");
    assert_eq!(uids(&v["friends"]), vec![alice.uid.clone()]);
    let ev = a.friends_event().await;
    assert_eq!((ev["change"].as_str(), ev["player"]["uid"].as_str()), (Some("accepted"), Some(bob.uid.as_str())));
    let v = friends_of(&stack, &sa).await;
    assert_eq!(uids(&v["friends"]), vec![bob.uid.clone()]);
    assert!(uids(&v["outgoing"]).is_empty() && uids(&v["incoming"]).is_empty());

    // A friends-only room: listed to the friends of its players (and its players), nobody else.
    let mut ga = OnlineClient::connect(stack.mm_addr, &creds(&alice), "0.1.0").expect("connect").0;
    ga.send(&RoomRequest::Create { public: true, ruleset: None, remembered: false });
    let code = ga.wait_for(WAIT, |m| m["type"] == "room-state").unwrap()["code"].as_str().unwrap().to_string();
    ga.send(&RoomRequest::Public { public: false, friends: true });
    let s = ga.wait_for(WAIT, |m| m["type"] == "room-state" && m["friends"] == true).expect("friends only");
    assert_eq!(s["public"], false);
    let body = rooms_until(&stack, &sb, |b| !b["rooms"].as_array().unwrap().is_empty()).await;
    assert_eq!(room_codes(&body), vec![code.clone()]);
    assert_eq!(body["rooms"][0]["friends"], true);
    assert!(body.get("friendRooms").is_none(), "never served: {body}");
    rooms_until(&stack, &sa, |b| room_codes(b) == vec![code.clone()]).await;
    let (_, body) = stack.get("/v1/rooms", Some(&sc)).await;
    assert!(room_codes(&body).is_empty(), "carol is nobody's friend: {body}");
    // Carol becomes dave's friend; dave joins: now she sees it (a friend of a player in it).
    stack.post("/v1/friends/requests", Some(&sc), json!({"to": dave.connect_code})).await;
    let (_, v) = stack.post("/v1/friends/requests", Some(&sd), json!({"to": carol.connect_code})).await;
    assert_eq!(v["status"], "friends");
    let mut gd = OnlineClient::connect(stack.mm_addr, &creds(&dave), "0.1.0").expect("connect").0;
    gd.send(&RoomRequest::Join { code: code.clone() });
    gd.wait_for(WAIT, |m| m["type"] == "room-state").expect("dave in");
    rooms_until(&stack, &sc, |b| room_codes(b) == vec![code.clone()]).await;
    // Private again: nobody's list.
    ga.send(&RoomRequest::Public { public: false, friends: false });
    rooms_until(&stack, &sb, |b| room_codes(b).is_empty()).await;
    rooms_until(&stack, &sc, |b| room_codes(b).is_empty()).await;

    // The ignore list: uploaded once (unknown and own uids skipped), then kept on the account.
    let (st, v) = stack.get("/v1/me/ignores", Some(&sc)).await;
    assert_eq!((st, v), (200, json!({"ignored": [], "imported": false})));
    let junk = "00000000-0000-4000-8000-000000000000";
    let (st, v) =
        stack.post("/v1/me/ignores/import", Some(&sc), json!({"uids": [alice.uid, junk, carol.uid, "x"]})).await;
    assert_eq!((st, v), (200, json!({"ignored": [alice.uid], "imported": true})));
    let (_, v) = stack.post("/v1/me/ignores/import", Some(&sc), json!({"uids": [bob.uid]})).await;
    assert_eq!(v["ignored"], json!([alice.uid]), "a second upload changes nothing");
    // A request from someone you ignore: stored, never shown, nobody told; the sender sees it sent.
    let mut c = connect(&stack, &sc, &carol).await;
    let (_, v) = stack.post("/v1/friends/requests", Some(&sa), json!({"to": carol.connect_code})).await;
    assert_eq!(v["status"], "sent");
    c.no_friends_event().await;
    assert!(uids(&friends_of(&stack, &sc).await["incoming"]).is_empty());
    // Unignored, it shows (it was kept).
    let (st, v) = stack.delete(&format!("/v1/me/ignores/{}", alice.uid), Some(&sc)).await;
    assert_eq!((st, v["ignored"].clone()), (200, json!([])));
    assert_eq!(uids(&friends_of(&stack, &sc).await["incoming"]), vec![alice.uid.clone()]);
    // Alice takes it back: gone, and carol's launcher reads its lists again.
    let (st, v) = stack.delete(&format!("/v1/friends/requests/{}", carol.uid), Some(&sa)).await;
    assert_eq!(st, 200);
    assert!(uids(&v["outgoing"]).is_empty());
    assert_eq!(c.friends_event().await["change"], "changed");
    assert!(uids(&friends_of(&stack, &sc).await["incoming"]).is_empty());
    // You can't ask someone you ignore.
    stack.put(&format!("/v1/me/ignores/{}", bob.uid), Some(&sc), json!({})).await;
    let (st, v) = stack.post("/v1/friends/requests", Some(&sc), json!({"to": bob.connect_code})).await;
    assert_eq!((st, v["error"]["code"].as_str()), (409, Some("ignoring")));

    // Ignoring a friend ends the friendship (both sides) and alice's launcher reads its lists.
    let (st, v) = stack.put(&format!("/v1/me/ignores/{}", alice.uid), Some(&sb), json!({})).await;
    assert_eq!((st, v["ignored"].clone()), (200, json!([alice.uid])));
    assert_eq!(a.friends_event().await["change"], "changed");
    assert!(uids(&friends_of(&stack, &sa).await["friends"]).is_empty());
    assert!(uids(&friends_of(&stack, &sb).await["friends"]).is_empty());

    // Removing a friend.
    let (st, v) = stack.delete(&format!("/v1/friends/{}", carol.uid), Some(&sd)).await;
    assert_eq!(st, 200);
    assert!(uids(&v["friends"]).is_empty());
    assert_eq!(c.friends_event().await["change"], "changed");
    assert!(uids(&friends_of(&stack, &sc).await["friends"]).is_empty());

    drop((ga, gd));
    stack.shutdown().await;
}
