//! End-to-end: SD card listings (`accounts::sd_cards`). A host's listing is stored once per card
//! root and fetched by a joiner; another card's listing, a malformed one, a bad root and a
//! logged-out call are refused.

use e2e::Stack;

/// A listing of a card of `chunks` chunks (one file over the first two), and its root in hex.
fn listing(chunks: u32, seed: u8) -> (Vec<u8>, String) {
    use blake2::digest::consts::U32;
    use blake2::{Blake2b, Digest};
    let mut canon = (u64::from(chunks) * 64 * 1024).to_le_bytes().to_vec();
    for i in 0..chunks {
        canon.push(1);
        canon.extend([seed.wrapping_add(i as u8); 16]);
    }
    let root: [u8; 32] = Blake2b::<U32>::digest(&canon).into();
    let mut out = (canon.len() as u32).to_le_bytes().to_vec();
    out.extend(&canon);
    // One file, "rex_/a.pac", 100 bytes, chunks 0-1.
    out.extend(1u32.to_le_bytes());
    out.extend(10u32.to_le_bytes());
    out.extend(b"rex_/a.pac");
    out.extend(100u32.to_le_bytes());
    out.extend(0u32.to_le_bytes());
    out.extend(1u32.to_le_bytes());
    out.extend(0u32.to_le_bytes());
    out.extend(2u32.to_le_bytes());
    let hex = root.iter().map(|b| format!("{b:02x}")).collect();
    (out, hex)
}

async fn put(s: &Stack, root: &str, token: Option<&str>, body: Vec<u8>) -> u16 {
    let mut req = s.http.put(s.url(&format!("/v1/sd-cards/{root}/listing"))).body(body);
    if let Some(t) = token {
        req = req.bearer_auth(t);
    }
    req.send().await.expect("http").status().as_u16()
}

async fn fetch(s: &Stack, root: &str, token: Option<&str>) -> (u16, Vec<u8>) {
    let mut req = s.http.get(s.url(&format!("/v1/sd-cards/{root}/listing")));
    if let Some(t) = token {
        req = req.bearer_auth(t);
    }
    let resp = req.send().await.expect("http");
    let status = resp.status().as_u16();
    (status, resp.bytes().await.unwrap_or_default().to_vec())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn host_uploads_joiner_fetches() {
    let s = Stack::start(Default::default()).await.unwrap();
    let (host, _) = s.create_player("host@x.test", "host", "HOST").await;
    let (joiner, _) = s.create_player("joiner@x.test", "joiner", "JOIN").await;
    let (body, root) = listing(4, 3);

    assert_eq!(fetch(&s, &root, Some(&joiner)).await.0, 404, "nothing yet");
    assert_eq!(put(&s, &root, Some(&host), body.clone()).await, 201);
    assert_eq!(put(&s, &root, Some(&joiner), body.clone()).await, 200, "already there");
    let (st, got) = fetch(&s, &root, Some(&joiner)).await;
    assert_eq!(st, 200);
    assert_eq!(got, body);
    let rows: i64 = sqlx::query_scalar("SELECT count(*) FROM sd_card_listings").fetch_one(&s.pool).await.unwrap();
    assert_eq!(rows, 1);

    // Another card's listing under this root, a malformed one, a bad root, no login.
    let (other, other_root) = listing(4, 9);
    assert_eq!(put(&s, &root, Some(&host), other.clone()).await, 400);
    assert_eq!(put(&s, &other_root, Some(&host), other[..other.len() - 2].to_vec()).await, 400);
    assert_eq!(put(&s, "NOTHEX", Some(&host), other.clone()).await, 400);
    assert_eq!(put(&s, &other_root, None, other.clone()).await, 401);
    assert_eq!(fetch(&s, &root, None).await.0, 401);
    assert_eq!(fetch(&s, &other_root, Some(&joiner)).await.0, 404);
}
