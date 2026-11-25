//! `admin speed summary/locked/show` against a fresh database, with rows written by mm's own
//! `record_speed_report` / `record_speed_check`. Runs the real `admin` executable.

use accounts::store;
use common::db::{self, SpeedCheckRecord, SpeedReportRecord};
use common::speed::{GameDelay, GamePerf, SpeedCheck};

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
    GamePerf { frames: 7000, fps, own_late, followed: 0.0, peer_fps: None, slow: false, peer_slow: false, delay: None }
}

async fn game(pool: &sqlx::PgPool, uid: uuid::Uuid, mode: &str, n: u32, perf: GamePerf, minutes_ago: i32) {
    let game_uid = format!("{:032x}", n as u128 + ((uid.as_u128() >> 64) << 32));
    game_on(pool, uid, mode, n, perf, minutes_ago, &game_uid, "game", ("dolphin", "")).await
}

/// [`game`] of `game_uid` (shared by the game's players), ended by `end_method`, on `engine`.
#[allow(clippy::too_many_arguments)]
async fn game_on(
    pool: &sqlx::PgPool,
    uid: uuid::Uuid,
    mode: &str,
    n: u32,
    perf: GamePerf,
    minutes_ago: i32,
    game_uid: &str,
    end_method: &str,
    (engine, engine_version): (&str, &str),
) {
    let r = SpeedReportRecord {
        uid,
        match_id: format!("mode.{mode}-{game_uid}-{n}"),
        mode: mode.into(),
        game_uid: game_uid.into(),
        set_game: (mode == "ranked").then_some(n),
        end_method: end_method.into(),
        perf,
        app_version: "0.1.95".into(),
        platform: "win32".into(),
        engine: engine.into(),
        engine_version: engine_version.into(),
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
    let r = SpeedCheckRecord {
        uid,
        check: c,
        app_version: "0.1.95".into(),
        platform: "win32".into(),
        engine: "dolphin".into(),
        engine_version: String::new(),
    };
    db::record_speed_check(pool, &r).await.unwrap();
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
    // (with the delay fields of builds from 2026-10-22)
    let delay = GameDelay {
        delay: 2,
        delay_profile: "normal".into(),
        delay_asked: "aggressive".into(),
        delay_fixed: false,
        rtt_start: 117.0,
        ping: 121.4,
        jitter: 6.0,
        rollbacks: 0.41,
        depth: 1.55,
        rollback_ms: 6.3,
        late_after_rollback: 0.08,
        hitches: 0.07,
        waits: 1,
        wait_ms: 20.0,
        resim_ms: 3.92,
    };
    game(&pool, bob.uid, "ranked", 2, GamePerf { delay: Some(Box::new(delay)), ..perf(59.9, 0.01) }, 40).await;
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
    // Ranked: 7 games, 4 slow (alice 2, bob 2), 2 players, none desynced; unranked: 1 of 1.
    assert!(text.contains("ranked     dolphin          7       4       2       0"), "{text}");
    assert!(text.contains("unranked   dolphin          1       1       1       0"), "{text}");
    // Checks: one slow (alice), one unsure (carol's single window).
    assert!(text.contains("win32      dolphin    D3D12            0       1       1       2"), "{text}");
    assert!(text.contains("would lock 1 player(s)"), "{text}");

    let (ok, text) = run(&url, &["speed", "show", "zz-speed-a@example.test"]).await;
    assert!(ok, "{text}");
    assert_eq!(text.matches("SLOW").count(), 2, "{text}");
    assert!(text.contains("[59.9, 45.0, 44.0] Slow"), "{text}");
    assert!(text.contains("3 event(s); 2 slow Ranked game(s) in a row: the rule would lock them"), "{text}");

    let (ok, text) = run(&url, &["speed", "show", "zz-speed-b@example.test"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("delay 2 normal (asked aggressive), ping 121 ms (jitter 6.0, 117 at the pick)"), "{text}");
    assert!(text.contains("rollbacks 41% 1.55 deep 6.3 ms, late after a rollback 8%, hitches 7%"), "{text}");
    assert!(text.contains("waits 1 (20 ms), re-run frame 3.92 ms"), "{text}");
    assert_eq!(text.matches(" delay ").count(), 1, "{text}");

    // A passing check unlocks her.
    check(&pool, alice.uid, &[59.9, 59.8, 59.9], 10).await;
    let (_, text) = run(&url, &["speed", "show", "zz-speed-a@example.test"]).await;
    assert!(text.contains("0 slow Ranked game(s) in a row: the rule would not lock them"), "{text}");
    let (_, text) = run(&url, &["speed", "locked"]).await;
    assert!(text.contains("nobody"), "{text}");
}

/// Speed and desync numbers split by engine (docs/design/brawl-online-core.md §10, "Which
/// engine"): per mode and engine, and per game by the engines of its players.
#[tokio::test]
async fn summary_splits_by_engine() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-engine-a@example.test", "x", "alice").await.unwrap();
    let bob = store::create_user(&pool, "zz-engine-b@example.test", "x", "bob").await.unwrap();
    let carol = store::create_user(&pool, "zz-engine-c@example.test", "x", "carol").await.unwrap();
    let dolphin = ("dolphin", "v0.2.5");
    let emissary = ("emissary", "0.7.0");
    let ok = perf(59.9, 0.01);
    // Alice (Dolphin) against Bob (the recomp): desynced (each side says so), then clean.
    for (uid, engine) in [(alice.uid, dolphin), (bob.uid, emissary)] {
        game_on(&pool, uid, "ranked", 1, ok.clone(), 50, "g1", "desync", engine).await;
        game_on(&pool, uid, "ranked", 2, ok.clone(), 40, "g2", "game", engine).await;
    }
    // Alice against Carol (both Dolphin): clean; Bob against Carol (only Bob reported it): slow.
    game_on(&pool, alice.uid, "unranked", 1, ok.clone(), 30, "g3", "game", dolphin).await;
    game_on(&pool, carol.uid, "unranked", 1, ok.clone(), 30, "g3", "game", dolphin).await;
    game_on(&pool, bob.uid, "unranked", 2, perf(44.0, 0.9), 20, "g4", "game", emissary).await;

    let (ok, text) = run(&url, &["speed", "summary"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("ranked     dolphin          2       0       0       1"), "{text}");
    assert!(text.contains("ranked     emissary         2       0       0       1"), "{text}");
    assert!(text.contains("unranked   dolphin          2       0       0       0"), "{text}");
    assert!(text.contains("unranked   emissary         1       1       1       0"), "{text}");
    // Games each side reported: the recomp's pair 1 of 2 desynced, Dolphin's 0 of 1; g4 (one
    // report) is not counted.
    assert!(text.contains("dolphin+emissary           2       1  50.00%"), "{text}");
    assert!(text.contains("dolphin+dolphin            1       0   0.00%"), "{text}");

    let (ok, text) = run(&url, &["speed", "summary", "--engine", "Emissary"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("engine emissary only"), "{text}");
    assert!(text.contains("ranked     emissary         2       0       0       1"), "{text}");
    assert!(!text.contains("dolphin          "), "{text}");
    assert!(text.contains("dolphin+emissary           2       1  50.00%"), "{text}");
    assert!(!text.contains("dolphin+dolphin"), "{text}");

    let (ok, text) = run(&url, &["speed", "show", "zz-engine-b@example.test"]).await;
    assert!(ok, "{text}");
    assert_eq!(text.matches("emissary 0.7.0").count(), 3, "{text}");
}
