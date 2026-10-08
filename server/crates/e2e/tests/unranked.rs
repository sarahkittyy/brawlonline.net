//! End-to-end Unranked: Postgres + accounts (HTTP) + mm (ENet/UDP) + fake game clients that
//! behave like Slippi's matchmaking client (`mmclient`), including its requeue after a failed
//! P2P connect.

use std::time::Duration;

use e2e::{creds, Stack, StackOptions, UserJson};
use mm::engine::EngineConfig;
use mm::ruleset::Rulesets;
use mmclient::{SearchOptions, SearchResult, Status};

/// P+ v3.2's legal stages (srStageKind), the Unranked list of `config/rulesets.json`.
const PPLUS_LEGAL: [u16; 15] =
    [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0C, 0x0D, 0x1C, 0x1F, 0x21, 0x23, 0x2D, 0x2E];

async fn search(opts: SearchOptions) -> SearchResult {
    tokio::task::spawn_blocking(move || mmclient::search(&opts)).await.unwrap().unwrap()
}

/// The engine as `mm` runs it: the repository's rulesets.
fn engine() -> EngineConfig {
    EngineConfig { rulesets: Rulesets::load(None).unwrap(), ..Default::default() }
}

fn opponent_uid(r: &SearchResult) -> String {
    let t = r.ticket_response.as_ref().expect("a ticket response");
    t.players.as_ref().unwrap().iter().find(|p| !p.is_local_player).unwrap().uid.clone()
}

async fn players(stack: &Stack, names: &[&str]) -> Vec<UserJson> {
    let mut v = vec![];
    for n in names {
        let (_, u) = stack.create_player(&format!("{n}@example.test"), n, &n[..2]).await;
        v.push(u);
    }
    v
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn two_strangers_meet_in_unranked() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["anna", "boris"]).await;

    let mut a = SearchOptions::unranked(stack.mm_addr, creds(&u[0]));
    let mut b = SearchOptions::unranked(stack.mm_addr, creds(&u[1]));
    a.punch = true;
    b.punch = true;
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    let ta = ra.ticket_response.as_ref().unwrap();
    let tb = rb.ticket_response.as_ref().unwrap();
    let match_id = ta.match_id.clone().unwrap();
    assert_eq!(ta.match_id, tb.match_id);
    assert!(match_id.starts_with("mode.unranked-"), "{match_id}");
    assert_ne!(ta.is_host, tb.is_host, "exactly one host");
    // The stage list of the Unranked ruleset, in the response as Slippi's server sends it.
    assert_eq!(ta.stages.as_deref(), Some(&PPLUS_LEGAL[..]));
    assert_eq!(tb.stages, ta.stages);
    assert_eq!((ta.items, tb.items), (Some(0), Some(0)));
    assert_eq!(opponent_uid(&ra), u[1].uid);
    assert_eq!(opponent_uid(&rb), u[0].uid);
    for (r, me) in [(&ra, &u[0]), (&rb, &u[1])] {
        let ps = r.ticket_response.as_ref().unwrap().players.as_ref().unwrap();
        assert_eq!(ps.len(), 2);
        let local = ps.iter().find(|p| p.is_local_player).unwrap();
        assert_eq!((local.uid.as_str(), local.connect_code.as_str()), (me.uid.as_str(), me.connect_code.as_str()));
        assert_eq!(local.ip_address, format!("127.0.0.1:{}", r.local_port));
        assert!(ps.iter().all(|p| p.chat_messages.len() == 16 && !p.is_bot));
        assert!(r.p2p.as_ref().unwrap().connected, "{r:?}");
    }

    // Recorded as an Unranked match with its stages and region.
    let mut row = None;
    for _ in 0..50 {
        row = sqlx::query_as::<_, (i16, Vec<i16>, Option<String>)>(
            "SELECT mode, stages, region FROM mm_matches WHERE match_id = $1",
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
    let (mode, stages, region) = row.expect("match recorded");
    assert_eq!(mode, 1);
    assert_eq!(stages, PPLUS_LEGAL.iter().map(|s| *s as i16).collect::<Vec<_>>());
    assert_eq!(region.as_deref(), Some(mm::region::DEFAULT_REGION));
    stack.shutdown().await;
}

/// First come, first served: the first two searches are paired, the third waits and, alone,
/// ends with an explicit error at the ticket TTL (the client itself would wait forever).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn unranked_is_first_come_first_served_and_expires_with_an_error() {
    let engine = EngineConfig { ticket_ttl: Duration::from_secs(3), ..engine() };
    let stack = Stack::start(StackOptions { engine, ..Default::default() }).await.unwrap();
    let u = players(&stack, &["cleo", "dmitri", "elif"]).await;

    let fa = tokio::spawn(search(SearchOptions::unranked(stack.mm_addr, creds(&u[0]))));
    tokio::time::sleep(Duration::from_millis(400)).await;
    let fb = tokio::spawn(search(SearchOptions::unranked(stack.mm_addr, creds(&u[1]))));
    tokio::time::sleep(Duration::from_millis(400)).await;
    let rc = search(SearchOptions::unranked(stack.mm_addr, creds(&u[2]))).await;
    let (ra, rb) = (fa.await.unwrap(), fb.await.unwrap());
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    assert_eq!(opponent_uid(&ra), u[1].uid);

    assert_eq!(rc.status, Status::TicketError, "{rc:?}");
    assert_eq!(rc.create_response.as_ref().unwrap().error, None, "the ticket was accepted first");
    assert_eq!(rc.error.as_deref(), Some(mm::messages::NO_OPPONENT));
    assert!(stack.mm_running());
    stack.shutdown().await;
}

/// The CSS's Z disconnects from mm: the ticket is gone at once and nobody is paired with it.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn cancelled_unranked_ticket_is_not_paired() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["fumi", "goran"]).await;

    let mut a = SearchOptions::unranked(stack.mm_addr, creds(&u[0]));
    a.match_timeout = Some(Duration::from_secs(1));
    assert_eq!(search(a).await.status, Status::MatchTimeout);
    let mut b = SearchOptions::unranked(stack.mm_addr, creds(&u[1]));
    b.match_timeout = Some(Duration::from_secs(1));
    let r = search(b).await;
    assert_eq!(r.status, Status::MatchTimeout, "{r:?}");

    tokio::time::sleep(Duration::from_secs(2)).await; // one ticket per account per 2 s
    let a = SearchOptions::unranked(stack.mm_addr, creds(&u[0]));
    let b = SearchOptions::unranked(stack.mm_addr, creds(&u[1]));
    let (ra, rb) = tokio::join!(search(a), search(b));
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(rb.status, Status::Matched, "{rb:?}");
    stack.shutdown().await;
}

/// Slippi's 1v1 requeue: A is matched with B, but B's game never connects (it vanished), so A's
/// 8 s P2P window fails and A searches again with a new ticket. B is searching again too. The
/// server does not put the two back together while others search: B gets C, A gets D, and A's
/// second P2P connect works.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn failed_connect_requeues_to_someone_else() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["hana", "ivan", "jun", "kofi"]).await;
    let t0 = tokio::time::Instant::now();

    let mut a = SearchOptions::unranked(stack.mm_addr, creds(&u[0]));
    a.punch = true;
    a.requeue = 1;
    a.match_timeout = Some(Duration::from_secs(40));
    let fa = tokio::spawn(search(a));
    // B is matched but never tries the P2P connect.
    let rb1 = search(SearchOptions::unranked(stack.mm_addr, creds(&u[1]))).await;
    assert_eq!(rb1.status, Status::Matched, "{rb1:?}");
    assert_eq!(opponent_uid(&rb1), u[0].uid);

    // B searches again (after the per-account ticket interval) and waits.
    tokio::time::sleep_until(t0 + Duration::from_secs(3)).await;
    let mut b2 = SearchOptions::unranked(stack.mm_addr, creds(&u[1]));
    b2.match_timeout = Some(Duration::from_secs(30));
    let fb2 = tokio::spawn(search(b2));
    // A requeues once its window fails (about 8-9 s in); C and D come later.
    tokio::time::sleep_until(t0 + Duration::from_secs(11)).await;
    let mut c = SearchOptions::unranked(stack.mm_addr, creds(&u[2]));
    c.match_timeout = Some(Duration::from_secs(30));
    let fc = tokio::spawn(search(c));
    tokio::time::sleep_until(t0 + Duration::from_secs(12)).await;
    let mut d = SearchOptions::unranked(stack.mm_addr, creds(&u[3]));
    d.punch = true;
    d.match_timeout = Some(Duration::from_secs(30));
    let rd = search(d).await;

    let ra = fa.await.unwrap();
    let (rb2, rc) = (fb2.await.unwrap(), fc.await.unwrap());
    assert_eq!(ra.failed_matches.len(), 1, "{ra:?}");
    let failed = &ra.failed_matches[0];
    assert!(failed.players.as_ref().unwrap().iter().any(|p| p.uid == u[1].uid));
    assert_eq!(failed.match_id, rb1.ticket_response.as_ref().unwrap().match_id);
    assert_eq!(ra.status, Status::Matched, "{ra:?}");
    assert_eq!(opponent_uid(&ra), u[3].uid, "A's new ticket gets D, not B again");
    assert!(ra.p2p.as_ref().unwrap().connected, "{ra:?}");
    assert_eq!(rd.status, Status::Matched, "{rd:?}");
    assert!(rd.p2p.as_ref().unwrap().connected, "{rd:?}");
    assert_eq!(rb2.status, Status::Matched, "{rb2:?}");
    assert_eq!(opponent_uid(&rb2), u[2].uid, "B's new ticket gets C");
    assert_eq!(rc.status, Status::Matched, "{rc:?}");
    stack.shutdown().await;
}

/// Alone together after a failed connect, the two are paired again, but only after the P2P
/// window plus the backoff (8 s + 5 s after the first match), not at once.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn failed_pair_alone_is_paired_again_after_the_backoff() {
    let stack = Stack::start(StackOptions { engine: engine(), ..Default::default() }).await.unwrap();
    let u = players(&stack, &["lena", "milo"]).await;
    let (r1a, r1b) = tokio::join!(
        search(SearchOptions::unranked(stack.mm_addr, creds(&u[0]))),
        search(SearchOptions::unranked(stack.mm_addr, creds(&u[1])))
    );
    assert_eq!((r1a.status.clone(), r1b.status), (Status::Matched, Status::Matched));
    let first = tokio::time::Instant::now();

    // Both come back (as after a failed 8 s P2P window).
    tokio::time::sleep(Duration::from_secs(3)).await;
    let (r2a, r2b) = tokio::join!(
        search(SearchOptions::unranked(stack.mm_addr, creds(&u[0]))),
        search(SearchOptions::unranked(stack.mm_addr, creds(&u[1])))
    );
    let waited = first.elapsed();
    assert_eq!(r2a.status, Status::Matched, "{r2a:?}");
    assert_eq!(r2b.status, Status::Matched, "{r2b:?}");
    assert_ne!(r2a.ticket_response.as_ref().unwrap().match_id, r1a.ticket_response.as_ref().unwrap().match_id);
    assert!(waited >= Duration::from_millis(12_500) && waited < Duration::from_secs(20), "{waited:?}");
    stack.shutdown().await;
}
