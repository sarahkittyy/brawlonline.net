//! `admin room rename TARGET [NAME]` names a host's rooms, by a room code open now (mm's status
//! says whose and which kind) or by the account, and audit-logs the old and new name; `room list`
//! shows the rooms with their names and hosts. Runs the real `admin` executable against a fresh
//! database and a stand-in for mm's status listener.

use accounts::store;

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

#[tokio::test]
async fn rename_rooms() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let alice = store::create_user(&pool, "zz-room-alice@example.test", "x", "alice").await.unwrap();
    let bob = store::create_user(&pool, "zz-room-bob@example.test", "x", "bob").await.unwrap();
    store::set_room_name(&pool, alice.uid, common::rooms::RoomKind::Pplus, Some("rude name")).await.unwrap();
    let names = |uid: uuid::Uuid| {
        let pool = pool.clone();
        async move {
            sqlx::query_as::<_, (Option<String>, Option<String>)>(
                "SELECT room_name, mods_room_name FROM users WHERE uid = $1",
            )
            .bind(uid)
            .fetch_one(&pool)
            .await
            .unwrap()
        }
    };

    // mm: alice hosts P+ room KFQB (bob is in it), bob hosts Brawl Mods room ZZTT.
    let status = serde_json::json!({
        "online": 2, "rooms": [], "updatedAt": "2026-11-12T12:00:00Z",
        "members": [
            {"uid": alice.uid.to_string(), "code": "KFQB", "players": 2, "openSlots": 4, "mode": "ffa",
             "status": "waiting", "joinable": true, "host": true, "name": "rude name"},
            {"uid": bob.uid.to_string(), "code": "KFQB", "players": 2, "openSlots": 4, "mode": "ffa",
             "status": "waiting", "joinable": true, "host": false, "name": "rude name"},
        ],
        "modMembers": [{"uid": bob.uid.to_string(), "code": "ZZTT", "host": true}],
    });
    let app = axum::Router::new().route("/status", axum::routing::get(move || async move { axum::Json(status) }));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let mm = format!("http://{}/status", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });

    let (ok, text) = run(&url, &["room", "list", "--mm-status-url", &mm]).await;
    assert!(ok, "{text}");
    assert!(text.contains("2 room(s)"), "{text}");
    let line =
        |code: &str| text.lines().find(|l| l.contains(code)).unwrap_or_else(|| panic!("{code}: {text}")).to_string();
    assert!(line("KFQB").starts_with("P+") && line("KFQB").contains(r#""rude name""#), "{text}");
    assert!(line("KFQB").contains("(alice)"), "{text}");
    assert!(line("ZZTT").starts_with("Mods") && line("ZZTT").contains("(no name)"), "{text}");
    assert!(line("ZZTT").contains("(bob)"), "{text}");

    // By room code (any case): the host's name of that kind, never a guest's.
    let (ok, text) = run(&url, &["room", "rename", "kfqb", "  Friday   doubles ", "--mm-status-url", &mm]).await;
    assert!(ok, "{text}");
    assert!(text.contains(r#"P+ room KFQB of"#) && text.contains(r#"from "rude name" to "Friday doubles""#), "{text}");
    assert_eq!(names(alice.uid).await, (Some("Friday doubles".into()), None));
    assert_eq!(names(bob.uid).await, (None, None));
    let (ok, text) = run(&url, &["room", "rename", "ZZTT", "Melee mods", "--mm-status-url", &mm]).await;
    assert!(ok, "{text}");
    assert_eq!(names(bob.uid).await, (None, Some("Melee mods".into())));

    // By account, and cleared with no name.
    let (ok, text) = run(&url, &["room", "rename", "zz-room-alice@example.test"]).await;
    assert!(ok, "{text}");
    assert!(text.contains(r#"from "Friday doubles" to (no name)"#), "{text}");
    assert_eq!(names(alice.uid).await, (None, None));
    let (ok, text) = run(&url, &["room", "rename", &bob.uid.to_string(), "", "--mods"]).await;
    assert!(ok, "{text}");
    assert_eq!(names(bob.uid).await, (None, None));

    // Refused, changing nothing: a room not open, a name too long or with a hidden character.
    for args in [
        vec!["room", "rename", "BBBB", "x", "--mm-status-url", &mm],
        vec!["room", "rename", "KFQB", "a name that is much longer than thirty-two", "--mm-status-url", &mm],
        vec!["room", "rename", "KFQB", "a\u{202E}b", "--mm-status-url", &mm],
    ] {
        let (ok, text) = run(&url, &args).await;
        assert!(!ok, "{args:?} accepted: {text}");
    }
    assert_eq!(names(alice.uid).await, (None, None));

    let audits: Vec<serde_json::Value> =
        sqlx::query_scalar("SELECT detail FROM audit_log WHERE action = 'user.room_name' ORDER BY id")
            .fetch_all(&pool)
            .await
            .unwrap();
    assert_eq!(audits.len(), 4);
    assert_eq!(
        audits[0],
        serde_json::json!({"kind": "pplus", "room": "KFQB", "from": "rude name", "to": "Friday doubles"})
    );
    assert_eq!(audits[3], serde_json::json!({"kind": "mods", "room": null, "from": "Melee mods", "to": null}));

    // `user show` has both names.
    let (ok, text) = run(&url, &["user", "show", "zz-room-bob@example.test"]).await;
    assert!(ok, "{text}");
    assert!(text.contains("room name:      (no name)"), "{text}");
    pool.close().await;
    e2e::drop_db(&name).await;
}
