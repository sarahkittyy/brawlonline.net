//! End-to-end: mm restarts (every deploy) while a Ranked set is under way (bug report #80,
//! 2026-10-21: after the restart every offer of the set's games was refused, "That match isn't
//! yours to stream"). Postgres + accounts + mm, then a second mm on the same database: it reads the
//! set back, records the players' presence again (offline until each game is back online), and
//! takes the set's next offer.

use std::time::{Duration, Instant};

use e2e::{creds, Stack, StackOptions, UserJson, TEST_SECRET};
use mm::engine::EngineConfig;
use mmclient::room::OnlineClient;
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::json;
use uuid::Uuid;

const WAIT: Duration = Duration::from_secs(10);

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

fn online(addr: std::net::SocketAddr, user: &UserJson) -> OnlineClient {
    let (client, hello) = OnlineClient::connect(addr, &creds(user), "0.1.0").expect("connect");
    assert_eq!(hello["type"], "hello-resp");
    client
}

/// Waits until `uid`'s `ranked_presence` row for `match_id` says `online`.
async fn presence_becomes(stack: &Stack, match_id: &str, uid: &str, online: bool) {
    let uid: Uuid = uid.parse().unwrap();
    let deadline = Instant::now() + WAIT;
    loop {
        let row: Option<bool> =
            sqlx::query_scalar("SELECT online FROM ranked_presence WHERE match_id = $1 AND uid = $2")
                .bind(match_id)
                .bind(uid)
                .fetch_optional(&stack.pool)
                .await
                .unwrap();
        if row == Some(online) {
            return;
        }
        assert!(Instant::now() < deadline, "presence of {uid} is {row:?}, not {online}");
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_set_under_way_goes_on_after_mm_restarts() {
    let mut stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (a, b) = (online(stack.mm_addr, &alice), online(stack.mm_addr, &bob));
    let (ra, rb) = tokio::join!(
        search(SearchOptions::ranked(stack.mm_addr, creds(&alice))),
        search(SearchOptions::ranked(stack.mm_addr, creds(&bob)))
    );
    assert_eq!((ra.status, rb.status), (Status::Matched, Status::Matched));
    let match_id = ra.ticket_response.as_ref().unwrap().match_id.clone().unwrap();
    presence_becomes(&stack, &match_id, &alice.uid, true).await;
    presence_becomes(&stack, &match_id, &bob.uid, true).await;

    // mm restarts: its matches were in memory only.
    let old = stack.mm.take().unwrap();
    tokio::task::spawn_blocking(move || old.stop()).await.unwrap();
    drop((a, b));
    let secret = common::playkey::PlayKeySecret::parse(TEST_SECRET).unwrap();
    let mm = mm::start(
        "127.0.0.1:0".parse().unwrap(),
        None,
        64,
        EngineConfig::default(),
        secret,
        stack.pool.clone(),
        tokio::runtime::Handle::current(),
    )
    .unwrap();
    let addr = mm.addr;
    stack.mm = Some(mm);

    // The set is read back; neither game is back yet.
    presence_becomes(&stack, &match_id, &alice.uid, false).await;
    presence_becomes(&stack, &match_id, &bob.uid, false).await;
    // Alice's game is back online and offers the set's game under way: a role, not a refusal.
    let alice_uid = alice.uid.clone();
    let mut a = tokio::task::spawn_blocking(move || online(addr, &alice)).await.unwrap();
    presence_becomes(&stack, &match_id, &alice_uid, true).await;
    let offer = json!({"type": "spectate-offer", "matchId": match_id, "gameUid": "cd".repeat(16), "game": 2});
    let reply = tokio::task::spawn_blocking(move || {
        a.send_json(&offer);
        a.wait_for(WAIT, |m| m["type"] == "spectate-role" || m["type"] == "spectate-error").unwrap()
    })
    .await
    .unwrap();
    assert_eq!(reply["type"], "spectate-role", "{reply}");
    assert_eq!(reply["role"], "primary", "a Ranked set is streamed for the record: {reply}");

    stack.shutdown().await;
}

/// mm restarts (every deploy) with a room open (bug report #178: the deploy at 05:58 UTC on
/// 2026-10-28 emptied every room, and the games that joined theirs again were told "Room not
/// found."): stopped as systemd stops it, mm saves its rooms; the next mm puts them back, and each
/// game that joins its room again has its own slot.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_room_is_there_after_mm_restarts() {
    use common::rooms::RoomRequest;
    let mut stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    let (a, b, code) = tokio::task::spawn_blocking({
        let (alice, bob, addr) = (alice.clone(), bob.clone(), stack.mm_addr);
        move || {
            let (mut a, mut b) = (online(addr, &alice), online(addr, &bob));
            a.send(&RoomRequest::Create { public: true, ruleset: None, remembered: false });
            let code = a.wait_for(WAIT, |m| m["type"] == "room-state").unwrap()["code"].as_str().unwrap().to_string();
            b.send(&RoomRequest::Join { code: code.clone() });
            b.wait_for(WAIT, |m| m["type"] == "room-state").unwrap();
            (a, b, code)
        }
    })
    .await
    .unwrap();

    // systemd stops mm (SIGTERM): the rooms are saved on the way out.
    let old = stack.mm.take().unwrap();
    tokio::task::spawn_blocking(move || {
        old.stop_switch().stop_keeping_rooms();
        old.join();
    })
    .await
    .unwrap();
    drop((a, b));
    let saved: i64 = sqlx::query_scalar("SELECT count(*) FROM mm_saved_rooms").fetch_one(&stack.pool).await.unwrap();
    assert_eq!(saved, 1);
    let secret = common::playkey::PlayKeySecret::parse(TEST_SECRET).unwrap();
    let mm = mm::start(
        "127.0.0.1:0".parse().unwrap(),
        None,
        64,
        EngineConfig::default(),
        secret,
        stack.pool.clone(),
        tokio::runtime::Handle::current(),
    )
    .unwrap();
    let addr = mm.addr;
    stack.mm = Some(mm);

    // Both games connect again and join the room, Bob first: each has their own slot, Alice is
    // still its host, and the saved rooms were taken.
    tokio::task::spawn_blocking(move || {
        let (mut a, mut b) = (online(addr, &alice), online(addr, &bob));
        b.send(&RoomRequest::Join { code: code.clone() });
        let s = b.wait_for(WAIT, |m| m["type"] == "room-state").unwrap();
        assert_eq!((s["code"].as_str(), s["you"].as_u64(), s["host"].as_u64()), (Some(code.as_str()), Some(2), Some(1)), "{s}");
        a.send(&RoomRequest::Join { code: code.clone() });
        let s = a.wait_for(WAIT, |m| m["type"] == "room-state").unwrap();
        assert_eq!((s["you"].as_u64(), s["slots"][1]["player"]["displayName"].as_str()), (Some(1), Some("bob")), "{s}");
    })
    .await
    .unwrap();
    let saved: i64 = sqlx::query_scalar("SELECT count(*) FROM mm_saved_rooms").fetch_one(&stack.pool).await.unwrap();
    assert_eq!(saved, 0);

    stack.shutdown().await;
}
