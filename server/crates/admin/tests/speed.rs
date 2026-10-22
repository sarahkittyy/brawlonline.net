//! `admin speed summary/locked/show` against a fresh database, with rows written by mm's own
//! `record_speed_report` / `record_speed_check`. Runs the real `admin` executable.

use accounts::store;
use common::db::{self, SpeedCheckRecord, SpeedReportRecord};
use common::speed::{GamePerf, SpeedCheck};

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

fn perf(fps: f64, own_late: f64) -> GamePerf {
    GamePerf { frames: 7000, fps, own_late, followed: 0.0, peer_fps: None, slow: false, peer_slow: false }
}

async fn game(pool: &sqlx::PgPool, uid: uuid::Uuid, mode: &str, n: u32, perf: GamePerf, minutes_ago: i32) {
    let r = SpeedReportRecord {
        uid,
        match_id: format!("mode.{mode}-{uid}-{n}"),
        mode: mode.into(),
        game_uid: format!("{:032x}", n as u128 + ((uid.as_u128() >> 64) << 32)),
        set_game: (mode == "ranked").then_some(n),
        end_method: "game".into(),
        perf,
        app_version: "0.1.95".into(),
        platform: "win32".into(),
    };
    db::record_speed_report(pool, &r).await.unwrap();
    // A second word on the same game changes nothing.
    db::record_speed_report(pool, &r).await.unwrap();
    sqlx::query("UPDATE speed_reports SET created_at = now() - make_interval(mins => $1) WHERE game_uid = $2")
        .bind(minutes_ago)
        .bind(&r.game_uid)
        .execute(pool)
        .await
        .unwrap();
}

async fn check(pool: &sqlx::PgPool, uid: uuid::Uuid, windows: &[f64], minutes_ago: i32) {
    let c = SpeedCheck { mode: "ranked".into(), windows: windows.to_vec(), gaps: 0, backend: "D3D12".into() };
    db::record_speed_check(pool, &SpeedCheckRecord { uid, check: c, app_version: "0.1.95".into(), platform: "win32".into() })
        .await
        .unwrap();
    sqlx::query(
        "UPDATE speed_checks SET created_at = now() - make_interval(mins => $1)
          WHERE id = (SELECT max(id) FROM speed_checks WHERE uid = $2)",
    )
    .bind(minutes_ago)
    .bind(uid)
    .execute(pool)
    .await
    .unwrap();
}

#[tokio::test]
async fn summary_locked_and_show() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-speed-a@example.test", "x", "alice").await.unwrap();
    let bob = store::create_user(&pool, "zz-speed-b@example.test", "x", "bob").await.unwrap();
    let carol = store::create_user(&pool, "zz-speed-c@example.test", "x", "carol").await.unwrap();

    let (ok, text) = run(&url, &["speed", "locked"]).await;
    assert!(ok && text.contains("nobody"), "{text}");

    // Alice: two slow Ranked games in a row (a slow check between them changes nothing).
    game(&pool, alice.uid, "ranked", 1, perf(44.0, 0.9), 50).await;
    check(&pool, alice.uid, &[59.9, 45.0, 44.0], 45).await;
    game(&pool, alice.uid, "ranked", 2, perf(45.0, 0.85), 40).await;
    // Bob: slow, then one at full speed, then slow: no two in a row.
    game(&pool, bob.uid, "ranked", 1, perf(46.0, 0.8), 50).await;
    game(&pool, bob.uid, "ranked", 2, perf(59.9, 0.01), 40).await;
    game(&pool, bob.uid, "ranked", 3, perf(46.0, 0.8), 30).await;
    // Carol: slowed by the other player's PC, never her own; and a slow Unranked game.
    game(&pool, carol.uid, "ranked", 1, GamePerf { followed: 0.9, peer_fps: Some(44.0), ..perf(44.0, 0.02) }, 50).await;
    game(&pool, carol.uid, "ranked", 2, GamePerf { followed: 0.9, peer_fps: Some(44.0), ..perf(44.0, 0.02) }, 40).await;
    game(&pool, carol.uid, "unranked", 3, perf(40.0, 0.9), 30).await;
    check(&pool, carol.uid, &[59.9], 20).await;

    let (ok, text) = run(&url, &["speed", "locked"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("(alice)") && text.contains("2 slow Ranked game(s) in a row"), "{text}");
    assert!(!text.contains("(bob)") && !text.contains("(carol)"), "{text}");

    let (ok, text) = run(&url, &["speed", "summary"]).await;
    assert!(ok, "{text}");
    // Ranked: 7 games, 4 slow (alice 2, bob 2), 2 players; unranked: 1 of 1.
    assert!(text.contains("ranked           7       4       2"), "{text}");
    assert!(text.contains("unranked         1       1       1"), "{text}");
    // Checks: one slow (alice), one unsure (carol's single window).
    assert!(text.contains("win32      D3D12            0       1       1       2"), "{text}");
    assert!(text.contains("would lock 1 player(s)"), "{text}");

    let (ok, text) = run(&url, &["speed", "show", "zz-speed-a@example.test"]).await;
    assert!(ok, "{text}");
    assert_eq!(text.matches("SLOW").count(), 2, "{text}");
    assert!(text.contains("[59.9, 45.0, 44.0] Slow"), "{text}");
    assert!(text.contains("3 event(s); 2 slow Ranked game(s) in a row: the rule would lock them"), "{text}");

    // A passing check unlocks her.
    check(&pool, alice.uid, &[59.9, 59.8, 59.9], 10).await;
    let (_, text) = run(&url, &["speed", "show", "zz-speed-a@example.test"]).await;
    assert!(text.contains("0 slow Ranked game(s) in a row: the rule would not lock them"), "{text}");
    let (_, text) = run(&url, &["speed", "locked"]).await;
    assert!(text.contains("nobody"), "{text}");
}
