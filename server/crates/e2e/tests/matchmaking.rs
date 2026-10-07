//! End-to-end: Postgres (docker) + accounts (HTTP) + mm (ENet/UDP) + two fake
//! game clients that behave like Slippi's matchmaking client.

use std::net::UdpSocket;
use std::time::Duration;

use e2e::{creds, Stack, StackOptions};
use mm::engine::EngineConfig;
use mmclient::{SearchOptions, SearchResult, Status};
use serde_json::{json, Value};

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

async fn raw(stack: &Stack, data: Vec<u8>) -> (Vec<Value>, bool) {
    let addr = stack.mm_addr;
    tokio::task::spawn_blocking(move || mmclient::send_raw_packet(addr, &data, Duration::from_secs(3)))
        .await
        .unwrap()
        .unwrap()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn two_friends_direct_connect_by_code() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "bo").await;
    assert!(alice.connect_code.starts_with("ALIC#"));
    assert!(bob.connect_code.starts_with("BO#"));

    let mut a = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let mut b = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code.to_lowercase());
    a.punch = true;
    b.punch = true;
    // Bob types the code as plain ASCII, Alice in full-width Shift-JIS like the game.
    b.encoding = mmclient::CodeEncoding::Ascii;
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");

    let ta = ra.ticket_response.as_ref().unwrap();
    let tb = rb.ticket_response.as_ref().unwrap();
    assert_eq!(ta.match_id, tb.match_id);
    assert!(ta.match_id.as_deref().unwrap().starts_with("mode.direct-"));
    assert_ne!(ta.is_host, tb.is_host, "exactly one host");
    let pa = ta.players.as_ref().unwrap();
    let pb = tb.players.as_ref().unwrap();
    assert_eq!(pa.len(), 2);
    for (x, y) in pa.iter().zip(pb) {
        assert_eq!(x.uid, y.uid);
        assert_eq!(x.connect_code, y.connect_code);
        assert_eq!(x.port, y.port);
        assert_eq!(x.ip_address, y.ip_address);
        assert_eq!(x.ip_address_lan, y.ip_address_lan);
        assert_ne!(x.is_local_player, y.is_local_player);
        assert_eq!(x.chat_messages.len(), 16);
    }
    // Each side sees the other's real external port (the hole-punch mapping)
    // and its own reported LAN address.
    let alice_entry = pa.iter().find(|p| p.uid == alice.uid).unwrap();
    let bob_entry = pa.iter().find(|p| p.uid == bob.uid).unwrap();
    assert!(alice_entry.is_local_player);
    assert_eq!(alice_entry.ip_address, format!("127.0.0.1:{}", ra.local_port));
    assert_eq!(bob_entry.ip_address, format!("127.0.0.1:{}", rb.local_port));
    assert_eq!(alice_entry.ip_address_lan, ra.lan_address);
    assert_eq!(alice_entry.display_name, "alice");
    assert_eq!(alice_entry.connect_code, alice.connect_code);
    // Same external IP, so Slippi's rule picks the LAN address.
    assert_eq!(ra.remote_addresses, vec![rb.lan_address.clone()]);
    assert_eq!(rb.remote_addresses, vec![ra.lan_address.clone()]);
    // And the clients actually reach each other from the punched port.
    assert!(ra.p2p.as_ref().unwrap().connected, "{ra:?}");
    assert!(rb.p2p.as_ref().unwrap().connected, "{rb:?}");

    // The match is recorded (asynchronously, by the database worker).
    let mut n = 0i64;
    for _ in 0..50 {
        n = sqlx::query_scalar("SELECT count(*) FROM mm_matches WHERE match_id = $1 AND status = 'ASSIGNED'")
            .bind(ta.match_id.as_deref().unwrap())
            .fetch_one(&stack.pool)
            .await
            .unwrap();
        if n == 1 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert_eq!(n, 1);
    assert!(stack.mm_running());
    stack.shutdown().await;
}

/// Slippi's client disconnects from mm itself right after `get-ticket-resp` and waits up to 3 s
/// for the server's answer before it binds the P2P port, so an unanswered disconnect costs 3 s of
/// the 8 s connect window (and the peer's first connects hit a port that cannot accept them).
/// The server used to start its own disconnect at the same moment; when the two crossed, the
/// client's was sometimes never answered (2 of 16 in a Dolphin run).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn client_disconnect_after_match_is_answered() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    for i in 0..10 {
        let (_, a) = stack.create_player(&format!("dca{i}@example.test"), "dca", "dca").await;
        let (_, b) = stack.create_player(&format!("dcb{i}@example.test"), "dcb", "dcb").await;
        let oa = SearchOptions::direct(stack.mm_addr, creds(&a), &b.connect_code);
        let ob = SearchOptions::direct(stack.mm_addr, creds(&b), &a.connect_code);
        let (ra, rb) = tokio::join!(search(oa), search(ob));
        for r in [&ra, &rb] {
            assert_eq!(r.status, Status::Matched, "{r:?}");
            let d = r.mm_disconnect.as_ref().unwrap();
            assert!(d.acknowledged && d.elapsed_ms < 1000, "round {i}: mm disconnect {d:?}");
        }
    }
    stack.shutdown().await;
}

/// A client that never disconnects after its match is still disconnected by the server.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn server_disconnects_a_lingering_client_after_a_match() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, a) = stack.create_player("linger-a@example.test", "la", "la").await;
    let (_, b) = stack.create_player("linger-b@example.test", "lb", "lb").await;
    let addr = stack.mm_addr;
    let (ca, b_code) = (creds(&a), b.connect_code.clone());
    let linger = tokio::task::spawn_blocking(move || {
        mmclient::search_and_linger(addr, ca, &b_code, Duration::from_secs(10)).unwrap()
    });
    let rb = search(SearchOptions::direct(stack.mm_addr, creds(&b), &a.connect_code)).await;
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    let (matched, disconnected_after) = linger.await.unwrap();
    assert!(matched);
    let after = disconnected_after.expect("the server never disconnected the lingering client");
    // After the server's grace period (1 s), not at once.
    assert!(after >= Duration::from_millis(800) && after <= Duration::from_secs(5), "{after:?}");
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn ticket_expiry_wrong_code_bad_key_and_unsupported_modes() {
    let engine = EngineConfig { ticket_ttl: Duration::from_secs(2), ..Default::default() };
    let stack = Stack::start(StackOptions { engine, ..Default::default() }).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "AL").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "BOB").await;
    let (_, carol) = stack.create_player("carol@example.test", "carol", "CARO").await;

    // Wrong code: Alice wants Bob, Bob wants Carol. Nobody matches, and both
    // tickets end with an explicit error at the TTL instead of hanging.
    let a = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let b = SearchOptions::direct(stack.mm_addr, creds(&bob), &carol.connect_code);
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::TicketError, "{ra:?}");
    assert_eq!(rb.status, Status::TicketError, "{rb:?}");
    assert_eq!(
        ra.error.as_deref(),
        Some(format!("Search timed out: {} did not connect within 1 minute.", bob.connect_code).as_str())
    );
    assert_eq!(ra.create_response.as_ref().unwrap().error, None, "ticket was accepted first");

    // A code that belongs to nobody behaves the same (no account enumeration).
    let r = search(SearchOptions::direct(stack.mm_addr, creds(&carol), "ZZZZ#999")).await;
    assert_eq!(r.status, Status::TicketError);

    // Malformed code.
    let r = search(SearchOptions::direct(stack.mm_addr, creds(&carol), "nonsense")).await;
    assert_eq!(r.status, Status::CreateError);
    assert_eq!(r.error.as_deref(), Some("Invalid connect code"));

    // Bad play key.
    let mut bad = creds(&alice);
    bad.play_key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA".into();
    tokio::time::sleep(Duration::from_secs(2)).await; // per-account ticket interval
    let r = search(SearchOptions::direct(stack.mm_addr, bad, &bob.connect_code)).await;
    assert_eq!(r.status, Status::CreateError);
    assert_eq!(r.error.as_deref(), Some("Invalid play key. Log in again in the launcher."));

    // Unknown uid.
    let mut ghost = creds(&alice);
    ghost.uid = uuid::Uuid::new_v4().to_string();
    let r = search(SearchOptions::direct(stack.mm_addr, ghost, &bob.connect_code)).await;
    assert_eq!(r.error.as_deref(), Some("Account not found. Log in again in the launcher."));

    // Queue modes are refused clearly in Phase 1.
    for (mode, name) in [(0u8, "Ranked"), (1, "Unranked"), (3, "Teams")] {
        let mut o = SearchOptions::direct(stack.mm_addr, creds(&carol), "");
        o.mode = mode;
        let r = search(o).await;
        assert_eq!(r.status, Status::CreateError);
        assert_eq!(r.error, Some(format!("{name} is not supported yet. Only Direct works for now.")));
    }

    // A banned account cannot queue: the ban rotates the play key, so the
    // old user.json is refused.
    let uid = uuid::Uuid::parse_str(&bob.uid).unwrap();
    accounts::store::set_ban(&stack.pool, uid, Some(accounts::store::permanent_ban()), Some("test")).await.unwrap();
    tokio::time::sleep(Duration::from_secs(2)).await;
    let r = search(SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code)).await;
    assert_eq!(r.status, Status::CreateError);
    assert_eq!(r.error.as_deref(), Some("Invalid play key. Log in again in the launcher."));

    assert!(stack.mm_running());
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn malformed_packets_do_not_crash_the_server() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "AL").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "BO").await;

    let cases: Vec<Vec<u8>> = vec![
        b"".to_vec(),
        b"not json at all".to_vec(),
        vec![0xff, 0xfe, 0x00, 0x80],
        b"[1,2,3]".to_vec(),
        br#"{"type":"create-ticket"}"#.to_vec(),
        br#"{"type":"create-ticket","user":{"uid":1,"playKey":[]},"search":{"mode":"two"}}"#.to_vec(),
        br#"{"type":"create-ticket","user":{"uid":"x","playKey":"y"},"search":{"mode":2,"connectCode":[999]}}"#
            .to_vec(),
        br#"{"type":"get-ticket"}"#.to_vec(),
        json!({"type": "create-ticket", "padding": "x".repeat(20_000)}).to_string().into_bytes(),
    ];
    for data in cases {
        let preview = String::from_utf8_lossy(&data[..data.len().min(60)]).to_string();
        let (replies, disconnected) = raw(&stack, data).await;
        assert_eq!(replies.len(), 1, "{preview}: {replies:?}");
        assert_eq!(replies[0]["type"], "create-ticket-resp", "{preview}");
        assert!(replies[0]["error"].as_str().is_some_and(|e| !e.is_empty()), "{preview}");
        assert!(disconnected, "server should close the connection: {preview}");
        assert!(stack.mm_running());
    }

    // Raw non-ENet UDP garbage at the port.
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    for len in [0usize, 1, 4, 48, 1400] {
        let junk: Vec<u8> = (0..len).map(|i| (i * 37 % 251) as u8).collect();
        sock.send_to(&junk, stack.mm_addr).unwrap();
    }
    tokio::time::sleep(Duration::from_millis(300)).await;
    assert!(stack.mm_running());

    // The server still pairs players afterwards.
    let a = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let b = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn client_cancel_then_search_again() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, alice) = stack.create_player("alice@example.test", "alice", "AL").await;
    let (_, bob) = stack.create_player("bob@example.test", "bob", "BO").await;

    // Alice searches alone and gives up (the CSS "press Z to cancel" path).
    let mut a = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    a.match_timeout = Some(Duration::from_secs(1));
    let r = search(a).await;
    assert_eq!(r.status, Status::MatchTimeout);

    // Bob searching now must not be paired with Alice's cancelled ticket.
    let mut b = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    b.match_timeout = Some(Duration::from_secs(1));
    let r = search(b).await;
    assert_eq!(r.status, Status::MatchTimeout, "{r:?}");

    tokio::time::sleep(Duration::from_secs(2)).await;
    let a = SearchOptions::direct(stack.mm_addr, creds(&alice), &bob.connect_code);
    let b = SearchOptions::direct(stack.mm_addr, creds(&bob), &alice.connect_code);
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::Matched);
    assert_eq!(rb.status, Status::Matched);
    stack.shutdown().await;
}
