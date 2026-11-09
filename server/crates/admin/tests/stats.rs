//! `admin stats` against a fresh database, with searches written by mm's own
//! `record_queue_search`, and a stand-in for mm's status listener. Runs the real `admin`
//! executable.

use common::db::{self, QueueSearchRecord};
use common::rooms::{MmStatus, QueueCounts};

async fn run(url: &str, args: &[&str]) -> (bool, String) {
    let out = tokio::process::Command::new(env!("CARGO_BIN_EXE_admin"))
        .args(args)
        .current_dir(std::env::temp_dir())
        .env("DATABASE_URL", url)
        .output()
        .await
        .unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    (out.status.success(), text)
}

/// A search of `mode` that ended `end_secs` after 03:00 UTC yesterday.
async fn search(pool: &sqlx::PgPool, uid: uuid::Uuid, mode: &'static str, waited_s: u32, matched: bool, end_secs: i32) {
    let r = QueueSearchRecord { uid, mode, waited_ms: waited_s * 1000, matched, requeue: false };
    db::record_queue_search(pool, &r).await.unwrap();
    sqlx::query(
        "UPDATE queue_searches
            SET ended_at = (date_trunc('day', now() AT TIME ZONE 'UTC') - interval '1 day' + interval '3 hours'
                            + make_interval(secs => $1)) AT TIME ZONE 'UTC'
          WHERE id = (SELECT max(id) FROM queue_searches)",
    )
    .bind(end_secs as f64)
    .execute(pool)
    .await
    .unwrap();
}

/// The table's line for an hour of the day (UTC).
fn hour_line(text: &str, h: u32) -> String {
    let h = format!("{h:02} ");
    text.lines().find(|l| l.len() > 3 && l[1..].starts_with(&h)).unwrap_or_else(|| panic!("no {h}: {text}")).to_string()
}

#[tokio::test]
async fn counts_and_estimated_waits() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();

    // mm's status listener.
    let status = MmStatus {
        online: 40,
        queues: QueueCounts { unranked: 3, ranked: 1, twos: 0 },
        playing: QueueCounts { unranked: 14, ranked: 6, twos: 4 },
        ..Default::default()
    };
    let app = axum::Router::new().route("/status", axum::routing::get(move || async move { axum::Json(status) }));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let mm = format!("http://{}/status", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });

    let (ok, text) = run(&url, &["stats", "--mm-status-url", &mm]).await;
    assert!(ok, "{text}");
    assert!(text.contains(" 40 online"), "{text}");
    assert!(text.contains("  Unranked     3 in queue    14 in game"), "{text}");
    assert!(text.contains("  Ranked       1 in queue     6 in game"), "{text}");
    assert!(text.contains("  2v2          0 in queue     4 in game"), "{text}");
    assert!(text.contains("last 14 days, 0 searches"), "{text}");

    // Unranked at 03 UTC: matched after 10, 20, 30 and 40 s; one given up after 50 s; and one
    // whose first match failed its connect (matched after 25 s, searched again 9 s later and
    // matched 15 s after that): one search of 49 s.
    let one = uuid::Uuid::new_v4;
    for (w, end) in [(10, 600), (20, 700), (30, 800), (40, 900)] {
        search(&pool, one(), "unranked", w, true, end).await;
    }
    search(&pool, one(), "unranked", 50, false, 1000).await;
    let retry = one();
    search(&pool, retry, "unranked", 25, true, 1100).await;
    search(&pool, retry, "unranked", 15, true, 1124).await;
    // Ranked at 03: too few searches for an estimate.
    search(&pool, one(), "ranked", 30, true, 600).await;

    let (ok, text) = run(&url, &["stats", "--days", "7", "--mm-status-url", &mm]).await;
    assert!(ok, "{text}");
    // Recorded since yesterday 03:09 (the first search): searches a day over the day or two since.
    assert!(text.contains("last 7 days, recorded since ") && text.contains(" 03:09 UTC, 7 searches"), "{text}");
    // Half matched by 30 s, 3 in 4 by 49 s (the given-up one still waiting at 50 s); Ranked: too
    // few for an estimate; 03 UTC is 22 at UTC-5.
    let line = hour_line(&text, 3);
    assert!(line.contains("     22 | ") && line.contains("   0:30   0:49|"), "{line}");
    assert!(line.ends_with("      -      -|   0.0      -      -"), "{line}");
    assert!(hour_line(&text, 4).contains("|   0.0      -      -|"), "{text}");

    // mm not answering: the estimates still come.
    let (ok, text) = run(&url, &["stats", "--offset", "1", "--mm-status-url", "http://127.0.0.1:9/status"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("mm's counts: not available"), "{text}");
    assert!(hour_line(&text, 3).contains("     04 |"), "{text}");

    pool.close().await;
    e2e::drop_db(&name).await;
}
