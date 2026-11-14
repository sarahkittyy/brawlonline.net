//! The launcher's admin tools (`/v1/admin/...`, `accounts/src/admin.rs`): only an account with
//! `role = 'admin'` gets them; mute, ban, rename, connect code, name lock and room names change what
//! `pp-admin` would, audit-logged with the admin as the actor. And the public profile's `dev` flag.
//! Postgres + accounts (HTTP) + mm (ENet/UDP), as `rooms.rs`.

use std::time::{Duration, Instant};

use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions, UserJson};
use mmclient::room::OnlineClient;
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(5);

async fn make_admin(s: &Stack, email: &str) {
    sqlx::query("UPDATE users SET role = 'admin' WHERE email = $1").bind(email).execute(&s.pool).await.unwrap();
}

async fn audits(s: &Stack, action: &str) -> Vec<(String, Value)> {
    sqlx::query_as::<_, (String, sqlx::types::Json<Value>)>(
        "SELECT actor, detail FROM audit_log WHERE action = $1 ORDER BY id",
    )
    .bind(action)
    .fetch_all(&s.pool)
    .await
    .unwrap()
    .into_iter()
    .map(|(a, d)| (a, d.0))
    .collect()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn admins_mute_ban_and_rename_players() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (sarah, sarah_json) = s.create_player("sarah@x.test", "sarah", "sara").await;
    let (alice, alice_json) = s.create_player("alice@x.test", "alice", "alic").await;
    let (_bob, bob_json) = s.create_player("bob@x.test", "bob", "bob").await;
    make_admin(&s, "sarah@x.test").await;
    let alice_uid = alice_json.uid.clone();
    let user = |uid: &str| format!("/v1/admin/users/{uid}");

    // Nobody else: not logged in 401, a player 403 `not_admin`, and nothing changed.
    for (st, body) in [s.get("/v1/admin/players", None).await, s.get(&user(&alice_uid), None).await] {
        assert_eq!(st, 401, "{body}");
    }
    for (st, body) in [
        s.get("/v1/admin/players", Some(&alice)).await,
        s.get(&user(&bob_json.uid), Some(&alice)).await,
        s.put(&format!("{}/muted", user(&bob_json.uid)), Some(&alice), json!({"muted": true})).await,
        s.put(&format!("{}/banned", user(&bob_json.uid)), Some(&alice), json!({"banned": true})).await,
        s.put(&user(&bob_json.uid), Some(&alice), json!({"displayName": "hacked"})).await,
        s.get("/v1/admin/codes?code=BOB%231", Some(&alice)).await,
        s.put("/v1/admin/rooms/KFQB/name", Some(&alice), json!({"name": "x"})).await,
    ] {
        assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_admin")), "{body}");
    }
    let (_, me) = s.get(&user(&bob_json.uid), Some(&sarah)).await;
    assert_eq!(me["displayName"], "bob");
    assert_eq!(me["muted"], false);

    // The players list (everyone with a code) and its search.
    let (st, body) = s.get("/v1/admin/players", Some(&sarah)).await;
    assert_eq!(st, 200, "{body}");
    let mut names: Vec<&str> =
        body["players"].as_array().unwrap().iter().map(|p| p["displayName"].as_str().unwrap()).collect();
    names.sort();
    assert_eq!(names, ["alice", "bob", "sarah"]);
    let (_, body) = s.get("/v1/admin/players?q=ALI", Some(&sarah)).await;
    assert_eq!(body["players"].as_array().unwrap().len(), 1, "{body}");
    assert_eq!(body["players"][0]["connectCode"], alice_json.connect_code.as_str());
    assert_eq!(body["players"][0]["banned"], false);
    let (_, body) = s.get("/v1/admin/players?q=%25", Some(&sarah)).await;
    assert_eq!(body["players"], json!([]), "a % is a %, not a wildcard");

    // Mute and unmute.
    let (st, body) = s.put(&format!("{}/muted", user(&alice_uid)), Some(&sarah), json!({"muted": true})).await;
    assert_eq!((st, &body["muted"]), (200, &json!(true)), "{body}");
    let muted: Option<chrono::DateTime<chrono::Utc>> =
        sqlx::query_scalar("SELECT chat_muted_at FROM users WHERE email = 'alice@x.test'")
            .fetch_one(&s.pool)
            .await
            .unwrap();
    assert!(muted.is_some());
    let (st, body) = s.put(&format!("{}/muted", user(&alice_uid)), Some(&sarah), json!({"muted": false})).await;
    assert_eq!((st, &body["muted"]), (200, &json!(false)), "{body}");
    let actor = format!("launcher:{}", sarah_json.uid);
    assert_eq!(audits(&s, "user.mute").await, vec![(actor.clone(), json!({}))]);
    assert_eq!(audits(&s, "user.unmute").await.len(), 1);

    // Another admin can't be muted or banned.
    for (st, body) in [
        s.put(&format!("{}/muted", user(&sarah_json.uid)), Some(&sarah), json!({"muted": true})).await,
        s.put(&format!("{}/banned", user(&sarah_json.uid)), Some(&sarah), json!({"banned": true})).await,
    ] {
        assert_eq!((st, body["error"]["code"].as_str()), (403, Some("target_admin")), "{body}");
    }

    // Rename, a new code and the name lock, all at once; a taken code changes nothing.
    let (st, body) =
        s.get(&format!("/v1/admin/codes?code={}", bob_json.connect_code.replace('#', "%23")), Some(&sarah)).await;
    assert_eq!((st, body["displayName"].as_str()), (200, Some("bob")), "{body}");
    let (_, body) = s.get("/v1/admin/codes?code=zzzz%230042", Some(&sarah)).await;
    assert_eq!(body, json!({"code": "ZZZZ#42", "uid": null, "displayName": null}));
    let (st, body) = s.get("/v1/admin/codes?code=nope", Some(&sarah)).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_connect_code")), "{body}");

    let (st, body) = s
        .put(&user(&alice_uid), Some(&sarah), json!({"displayName": "alice2", "connectCode": bob_json.connect_code}))
        .await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("code_taken")), "{body}");
    assert!(body["error"]["message"].as_str().unwrap().contains("bob"), "{body}");
    let (_, body) = s.get(&user(&alice_uid), Some(&sarah)).await;
    assert_eq!(body["displayName"], "alice", "nothing changed: {body}");
    let (st, body) = s
        .put(
            &user(&alice_uid),
            Some(&sarah),
            json!({"displayName": "alice2", "connectCode": "nice#0007", "nameLocked": true}),
        )
        .await;
    assert_eq!(st, 200, "{body}");
    assert_eq!(
        (body["displayName"].as_str(), body["connectCode"].as_str(), &body["nameLocked"]),
        (Some("alice2"), Some("NICE#7"), &json!(true))
    );
    // Locked: alice's own rename is refused.
    let (st, body) = s.post("/v1/me/rename", Some(&alice), json!({"displayName": "free"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("rename_locked")), "{body}");
    let (st, body) = s.put(&user(&alice_uid), Some(&sarah), json!({"nameLocked": false})).await;
    assert_eq!((st, &body["nameLocked"]), (200, &json!(false)), "{body}");
    let (st, body) = s.put(&user(&alice_uid), Some(&sarah), json!({"displayName": ""})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_display_name")), "{body}");
    assert_eq!(audits(&s, "user.rename").await, vec![(actor.clone(), json!({"from": "alice", "to": "alice2"}))]);
    assert_eq!(audits(&s, "user.set_code").await[0].1["to"], "NICE#7");
    assert_eq!(audits(&s, "user.lock_name").await.len(), 1);
    assert_eq!(audits(&s, "user.unlock_name").await.len(), 1);

    // Ban: alice's session ends; unban.
    let (st, body) = s.put(&format!("{}/banned", user(&alice_uid)), Some(&sarah), json!({"banned": true})).await;
    assert_eq!((st, &body["banned"]), (200, &json!(true)), "{body}");
    let (st, _) = s.get("/v1/me", Some(&alice)).await;
    assert_eq!(st, 401);
    let (_, body) = s.get("/v1/admin/players?q=alice2", Some(&sarah)).await;
    assert_eq!(body["players"][0]["banned"], true, "{body}");
    let (st, body) = s.put(&format!("{}/banned", user(&alice_uid)), Some(&sarah), json!({"banned": false})).await;
    assert_eq!((st, &body["banned"]), (200, &json!(false)), "{body}");
    assert_eq!(audits(&s, "user.ban").await.len(), 1);
    assert_eq!(audits(&s, "user.unban").await.len(), 1);

    // The developer's mark: on the admin's profile (launcher and web page), nobody else's.
    let (_, p) = s.get(&format!("/v1/profiles/{}", sarah_json.uid), None).await;
    assert_eq!(p["dev"], true, "{p}");
    let (_, p) = s.get(&format!("/v1/profiles/{}", bob_json.uid), None).await;
    assert_eq!(p["dev"], false, "{p}");
    let slug = sarah_json.connect_code.to_lowercase().replace('#', "-");
    let html = s.http.get(s.url(&format!("/profile/{slug}"))).send().await.unwrap().text().await.unwrap();
    assert!(html.contains("<span class=\"dev\" title=\"Dev\""), "{html}");
    let slug = bob_json.connect_code.to_lowercase().replace('#', "-");
    let html = s.http.get(s.url(&format!("/profile/{slug}"))).send().await.unwrap().text().await.unwrap();
    assert!(!html.contains("class=\"dev\""), "{html}");
}

fn online(stack: &Stack, user: &UserJson) -> OnlineClient {
    let (client, hello) = OnlineClient::connect(stack.mm_addr, &creds(user), "0.1.0").expect("connect");
    assert_eq!(hello, json!({"type": "hello-resp", "gameAlive": true}));
    client
}

/// `GET /v1/rooms` until `pred` holds.
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

/// An admin names someone else's open room as its host would: the host's account keeps the name and
/// the room shows it.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn admins_rename_any_room() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (sa, alice) = stack.create_player("alice@example.test", "alice", "alic").await;
    let (sarah, _) = stack.create_player("sarah@example.test", "sarah", "sara").await;
    make_admin(&stack, "sarah@example.test").await;
    let mut a = online(&stack, &alice);
    a.send(&RoomRequest::Create { public: true, ruleset: None, remembered: false });
    let code =
        a.wait_for(WAIT, |m| m["type"] == "room-state").expect("room-state")["code"].as_str().unwrap().to_string();
    rooms_until(&stack, &sa, |r| r["yourRoom"]["code"] == code.as_str()).await;

    let path = format!("/v1/admin/rooms/{}/name", code.to_lowercase());
    let (st, body) = stack.put(&path, Some(&sa), json!({"name": "mine"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_admin")), "{body}");
    let (st, body) = stack.put(&path, Some(&sarah), json!({"name": "  be   nice "})).await;
    assert_eq!((st, &body), (200, &json!({"kind": "pplus", "name": "be nice"})));
    rooms_until(&stack, &sarah, |r| r["rooms"][0]["name"] == "be nice").await;
    let (st, body) = stack.put("/v1/admin/rooms/BBBB/name", Some(&sarah), json!({"name": "x"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (404, Some("unknown_room")), "{body}");
    let (st, body) = stack.put(&path, Some(&sarah), json!({"name": "x".repeat(33)})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_room_name")), "{body}");
    let audit = audits(&stack, "user.room_name").await;
    assert_eq!(audit.len(), 1);
    assert_eq!(audit[0].1, json!({"kind": "pplus", "room": code, "from": null, "to": "be nice"}));
}
