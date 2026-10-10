//! End-to-end: chat (`docs/chat-protocol.md`). Postgres + accounts (HTTP) + mm (ENet/UDP),
//! accounts made over HTTP, and games that keep an online connection with a chat identity key and
//! encrypt, sign and check messages like Dolphin does (`mmclient::chat`).

use std::time::{Duration, Instant};

use common::chat::{self, ChatRequest, ReportedMessage};
use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mm::messages as msg;
use mmclient::chat::{ChatSession, Identity};
use mmclient::room::{OnlineClient, Received};
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::{json, Value};
use uuid::Uuid;

const WAIT: Duration = Duration::from_secs(5);

/// A running game that can chat.
struct Chatter {
    client: OnlineClient,
    chat: ChatSession,
    uid: String,
}

impl Chatter {
    fn connect(stack: &Stack, user: &UserJson) -> Chatter {
        let chat = ChatSession::new(Identity::generate());
        let key = chat.identity().public_hex();
        let (client, hello) =
            OnlineClient::connect_with_chat(stack.mm_addr, &creds(user), "0.1.0", "", &key).expect("connect");
        assert_eq!(hello, json!({"type": "hello-resp"}));
        Chatter { client, chat, uid: user.uid.clone() }
    }

    /// Feeds what mm sends to the chat session (sending what it answers, its group keys) until
    /// `done` accepts a message from mm or a chat event; returns that one.
    fn until(&mut self, what: &str, mut done: impl FnMut(&ChatSession, &Value) -> bool) -> Value {
        let deadline = Instant::now() + WAIT;
        let mut seen = vec![];
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self.client.recv(left.min(Duration::from_millis(50))) {
                Received::Message(m) => {
                    seen.push(m.clone());
                    let (events, requests) = self.chat.on_message(&m);
                    for r in &requests {
                        self.client.send_json(r);
                    }
                    if done(&self.chat, &m) {
                        return m;
                    }
                    if let Some(e) = events.into_iter().find(|e| done(&self.chat, e)) {
                        return e;
                    }
                }
                Received::Disconnected => panic!("{what}: mm disconnected"),
                Received::Timeout => {}
            }
            assert!(Instant::now() < deadline, "{what}: timed out; saw {seen:?}");
        }
    }

    /// Waits until exactly these members can read what this game sends.
    fn readers(&mut self, uids: &[&str]) {
        let mut want: Vec<String> = uids.iter().map(|u| u.to_string()).collect();
        want.sort();
        if sorted(self.chat.readers()) == want {
            return;
        }
        self.until("readers", |c, _| sorted(c.readers()) == want);
    }

    /// Everything that arrives within `wait` (fed to the chat session), messages and events.
    fn quiet(&mut self, wait: Duration) -> Vec<Value> {
        let mut all = vec![];
        for m in self.client.drain(wait) {
            let (events, requests) = self.chat.on_message(&m);
            for r in &requests {
                self.client.send_json(r);
            }
            all.push(m);
            all.extend(events);
        }
        all
    }

    fn say(&mut self, text: &str) -> Value {
        let (req, _) = self.chat.compose(text).expect("compose");
        self.client.send_json(&req);
        req
    }

    /// The next decrypted message (`mmclient-chat-msg`).
    fn heard(&mut self) -> Value {
        self.until("chat message", |_, m| {
            assert_ne!(m["type"], "mmclient-chat-drop", "dropped: {m}");
            m["type"] == "mmclient-chat-msg"
        })
    }
}

/// Lets every game run (each publishes its group key when mm puts it in a group) until each can
/// send to all the others. The games are driven one after another, so none may wait alone.
fn keys_exchanged(games: &mut [&mut Chatter]) {
    let uids: Vec<String> = games.iter().map(|g| g.uid.clone()).collect();
    let deadline = Instant::now() + WAIT;
    loop {
        let mut done = true;
        for g in games.iter_mut() {
            g.quiet(Duration::from_millis(20));
            let others: Vec<String> = sorted(uids.iter().filter(|u| **u != g.uid).cloned().collect());
            done &= sorted(g.chat.readers()) == others;
        }
        if done {
            return;
        }
        assert!(Instant::now() < deadline, "group keys were not exchanged");
    }
}

fn sorted(mut v: Vec<String>) -> Vec<String> {
    v.sort();
    v
}

fn room_code(c: &mut Chatter) -> String {
    let s = c.until("room-state", |_, m| m["type"] == "room-state");
    s["code"].as_str().unwrap().to_string()
}

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

/// Polls the database until `sql` (one count) gives `want`.
async fn count_until(stack: &Stack, sql: &str, want: i64) {
    let deadline = Instant::now() + WAIT;
    loop {
        let n: i64 = sqlx::query_scalar(sql).fetch_one(&stack.pool).await.unwrap();
        if n == want {
            return;
        }
        assert!(Instant::now() < deadline, "{sql}: {n}, not {want}");
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_room_chats_end_to_end_and_a_kicked_player_hears_nothing_more() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (_, carol) = stack.create_player("carol@example.test", "carol", "car").await;
    let (mut a, mut b, mut c) =
        (Chatter::connect(&stack, &alice), Chatter::connect(&stack, &bob), Chatter::connect(&stack, &carol));
    // mm records each identity key it accepted.
    count_until(&stack, "SELECT count(*) FROM chat_identity_keys", 3).await;

    a.client.send(&RoomRequest::Create { public: true });
    let code = room_code(&mut a);
    a.client.send(&RoomRequest::Slot { slot: 3, open: true });
    b.client.send(&RoomRequest::Join { code: code.clone() });
    c.client.send(&RoomRequest::Join { code: code.clone() });
    keys_exchanged(&mut [&mut a, &mut b, &mut c]);
    let group = a.chat.group().unwrap().to_string();
    assert!(group.starts_with(&format!("room-{code}-")), "{group}");

    // Every member hears every other, decrypted, checked and cleaned.
    a.say("hello ｗｏｒｌｄ ねこ");
    for x in [&mut b, &mut c] {
        let m = x.heard();
        assert_eq!(
            (m["from"].as_str(), m["text"].as_str(), m["seq"].as_u64()),
            (Some(alice.uid.as_str()), Some("hello ｗｏｒｌｄ ねこ"), Some(1))
        );
        assert_eq!(m["displayName"], "alice");
    }
    b.say("hi \u{202e}all");
    for x in [&mut a, &mut c] {
        assert_eq!(x.heard()["text"], "hi all");
    }

    // The host closes carol's slot: carol is out of the chat at once.
    a.client.send(&RoomRequest::Slot { slot: 3, open: false });
    c.until("chat-group null", |_, m| *m == json!({"type": "chat-group", "group": null}));
    assert_eq!(c.chat.group(), None);
    a.readers(&[&b.uid]);
    b.readers(&[&a.uid]);
    a.say("after the kick");
    assert_eq!(b.heard()["text"], "after the kick");
    assert!(c.quiet(Duration::from_millis(500)).iter().all(|m| m["type"] != "chat-msg"));
    // A box for her anyway is refused by mm, and nothing goes out.
    let forged =
        ChatRequest::Send { group: group.clone(), boxes: vec![(carol.uid.clone(), vec![0; chat::MIN_BOX_BYTES])] };
    a.client.send_json(&forged.to_json());
    let e = a.until("chat-error", |_, m| m["type"] == "chat-error");
    assert_eq!(e, json!({"type": "chat-error", "op": "chat-send", "error": msg::CHAT_NOT_A_MEMBER}));
    assert!(c.quiet(Duration::from_millis(300)).iter().all(|m| m["type"] != "chat-msg"));
    // Nor can she publish a key or send in the room's group.
    let key = ChatRequest::Key { group: group.clone(), kx: [1; 32], sig: [2; 64] };
    c.client.send_json(&key.to_json());
    let e = c.until("chat-error", |_, m| m["type"] == "chat-error");
    assert_eq!(e["error"], msg::CHAT_NOT_IN_GROUP);

    // Bob's game closes: alice is alone, nobody to send to.
    b.client.close();
    a.readers(&[]);
    assert!(a.chat.compose("anyone?").is_err());
    a.client.close();
    c.client.close();
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_direct_match_has_a_chat_until_a_new_search() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (mut a, mut b) = (Chatter::connect(&stack, &alice), Chatter::connect(&stack, &bob));

    // The games search from their P2P ports while their online connections stay open.
    let oa = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let ob = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    let (ra, rb) = tokio::join!(search(oa), search(ob));
    assert_eq!((ra.status, rb.status), (Status::Matched, Status::Matched));
    let match_id = ra.ticket_response.as_ref().unwrap().match_id.clone().unwrap();
    for x in [&mut a, &mut b] {
        let g = x.until("match group", |_, m| m["type"] == "chat-group" && m["kind"] == "match");
        assert_eq!(g["group"], match_id.as_str());
    }
    keys_exchanged(&mut [&mut a, &mut b]);
    b.say("ggs");
    assert_eq!(a.heard()["text"], "ggs");
    a.say("one more");
    assert_eq!(b.heard()["text"], "one more");

    // Alice searches again (after the per-account ticket limit): her match chat ends, bob's
    // group is left with him alone.
    tokio::time::sleep(Duration::from_millis(2100)).await;
    let again = SearchOptions {
        match_timeout: Some(Duration::from_secs(2)),
        ..SearchOptions::direct(stack.mm_addr, creds(&alice), "ZZZZ#999")
    };
    let pending = tokio::spawn(search(again));
    a.until("chat-group null", |_, m| *m == json!({"type": "chat-group", "group": null}));
    let g = b.until("group of one", |_, m| m["type"] == "chat-group");
    assert_eq!(g["members"].as_array().unwrap().len(), 1);
    assert!(b.chat.readers().is_empty());
    assert_eq!(pending.await.unwrap().status, Status::MatchTimeout);
    // Bob's session ends (Dolphin sends chat-leave): the group is gone.
    b.client.send_json(&b.chat.leave().unwrap());
    b.until("chat-group null", |_, m| *m == json!({"type": "chat-group", "group": null}));
    a.client.close();
    b.client.close();
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_report_is_stored_with_the_messages_that_verify() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (mut a, mut b) = (Chatter::connect(&stack, &alice), Chatter::connect(&stack, &bob));
    a.client.send(&RoomRequest::Create { public: false });
    let code = room_code(&mut a);
    b.client.send(&RoomRequest::Join { code });
    keys_exchanged(&mut [&mut a, &mut b]);
    let group = a.chat.group().unwrap().to_string();
    b.say("you  are\u{200b} bad");
    assert_eq!(a.heard()["text"], "you are bad");

    // Alice reports bob with what he sent, plus a message she made up (signed with a key that is
    // not his): the first verifies, the second does not.
    let mut messages = a.chat.received_from(&bob.uid);
    assert_eq!(messages.len(), 1);
    let made_up = "I cheat";
    messages.push(ReportedMessage {
        seq: 2,
        text: made_up.into(),
        sig: chat::sign_message(&[7; 32], &group, &bob.uid, 2, made_up.as_bytes()),
    });
    let report = ChatRequest::Report { group: group.clone(), from: bob.uid.clone(), reason: "rude\n".into(), messages };
    a.client.send_json(&report.to_json());
    let r = a.until("chat-reported", |_, m| m["type"] == "chat-reported");
    assert_eq!(r, json!({"type": "chat-reported", "from": bob.uid}));
    count_until(&stack, "SELECT count(*) FROM chat_reports", 1).await;
    let (reporter, reported, group_id, reason, verified, total, messages): (
        Uuid,
        Uuid,
        String,
        String,
        i32,
        i32,
        Value,
    ) = sqlx::query_as("SELECT reporter, reported, group_id, reason, verified, total, messages FROM chat_reports")
        .fetch_one(&stack.pool)
        .await
        .unwrap();
    assert_eq!((reporter.to_string(), reported.to_string()), (alice.uid.clone(), bob.uid.clone()));
    assert_eq!((group_id, reason, verified, total), (group, "rude".to_string(), 1, 2));
    assert_eq!(messages[0]["text"], "you are bad");
    assert_eq!(messages[0]["verified"], true);
    assert_eq!(messages[1]["text"], made_up);
    assert_eq!(messages[1]["verified"], false);

    // Malformed reports are refused by mm, not stored.
    let bad = json!({"type": "chat-report", "group": "room-X-1", "from": "bob", "messages": []});
    a.client.send_json(&bad);
    let e = a.until("chat-error", |_, m| m["type"] == "chat-error");
    assert_eq!(e, json!({"type": "chat-error", "op": "chat-report", "error": msg::CHAT_INVALID}));
    a.client.close();
    b.client.close();
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sending_too_fast_is_refused() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (mut a, mut b) = (Chatter::connect(&stack, &alice), Chatter::connect(&stack, &bob));
    a.client.send(&RoomRequest::Create { public: true });
    let code = room_code(&mut a);
    b.client.send(&RoomRequest::Join { code });
    keys_exchanged(&mut [&mut a, &mut b]);
    for i in 0..6 {
        a.say(&format!("spam {i}"));
    }
    let e = a.until("chat-error", |_, m| m["type"] == "chat-error");
    assert_eq!(e, json!({"type": "chat-error", "op": "chat-send", "error": msg::CHAT_TOO_FAST}));
    let heard: Vec<String> = (0..5).map(|_| b.heard()["text"].as_str().unwrap().to_string()).collect();
    assert_eq!(heard, (0..5).map(|i| format!("spam {i}")).collect::<Vec<_>>());
    assert!(b.quiet(Duration::from_millis(500)).iter().all(|m| m["type"] != "chat-msg"));
    // A game without a chat key can be in the room but not in its chat.
    let (_, carol) = stack.create_player("carol@example.test", "carol", "car").await;
    let (mut c, _) = OnlineClient::connect(stack.mm_addr, &creds(&carol), "0.1.0").unwrap();
    c.send_json(&json!({"type": "chat-leave", "group": "room-X-1"}));
    let e = c.wait_for(WAIT, |m| m["type"] == "chat-error").unwrap();
    assert_eq!(e["error"], msg::CHAT_UPDATE);
    a.client.close();
    b.client.close();
    c.close();
    stack.shutdown().await;
}
