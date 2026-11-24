//! Twitch (docs/design/twitch-live.md) against a stand-in for Twitch: connecting through the
//! sign-in's code, the profile's handle following it, and a connected player's stream listed only
//! while they play.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use axum::extract::{Form, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::routing::{get, post};
use axum::{Json, Router};
use common::rooms::RoomRequest;
use e2e::{creds, Stack, StackOptions};
use mmclient::room::OnlineClient;
use serde_json::{json, Value};

const WAIT: Duration = Duration::from_secs(8);

/// What the stand-in saw: tokens revoked.
#[derive(Default)]
struct Seen {
    revoked: Vec<String>,
}

type Shared = Arc<Mutex<Seen>>;

/// Twitch's token endpoint: an app token for client credentials, a user token for the code "good".
async fn token(Form(f): Form<HashMap<String, String>>) -> (StatusCode, Json<Value>) {
    if f.get("client_id").map(String::as_str) != Some("e2e-client")
        || f.get("client_secret").map(String::as_str) != Some("e2e-secret")
    {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "bad client"})));
    }
    match (f.get("grant_type").map(String::as_str), f.get("code").map(String::as_str)) {
        (Some("client_credentials"), _) => (StatusCode::OK, Json(json!({"access_token": "app", "expires_in": 3600}))),
        (Some("authorization_code"), Some("good")) => {
            assert!(f["redirect_uri"].ends_with("/v1/twitch/callback"), "{f:?}");
            (StatusCode::OK, Json(json!({"access_token": "user", "expires_in": 3600})))
        }
        _ => (StatusCode::BAD_REQUEST, Json(json!({"message": "bad code"}))),
    }
}

async fn revoke(State(seen): State<Shared>, Form(f): Form<HashMap<String, String>>) -> StatusCode {
    seen.lock().unwrap().revoked.push(f.get("token").cloned().unwrap_or_default());
    StatusCode::OK
}

fn bearer(h: &HeaderMap) -> &str {
    h.get("authorization").and_then(|v| v.to_str().ok()).unwrap_or("")
}

async fn users(headers: HeaderMap) -> (StatusCode, Json<Value>) {
    if bearer(&headers) != "Bearer user" {
        return (StatusCode::UNAUTHORIZED, Json(json!({})));
    }
    (StatusCode::OK, Json(json!({"data": [{"id": "777", "login": "knuxplays", "display_name": "KnuxPlays"}]})))
}

/// User 777 is always live (Melee's category: the category doesn't matter); 888 never is.
async fn streams(headers: HeaderMap, Query(q): Query<Vec<(String, String)>>) -> (StatusCode, Json<Value>) {
    if bearer(&headers) != "Bearer app" {
        return (StatusCode::UNAUTHORIZED, Json(json!({})));
    }
    let asked: Vec<&str> = q.iter().filter(|(k, _)| k == "user_id").map(|(_, v)| v.as_str()).collect();
    let data: Vec<Value> = asked
        .iter()
        .filter(|id| **id == "777")
        .map(|_| {
            json!({"user_id": "777", "user_login": "knuxplays", "user_name": "KnuxPlays", "type": "live",
                   "game_name": "Super Smash Bros. Melee", "title": "brawl online ranked", "viewer_count": 23,
                   "started_at": "2026-11-24T10:00:00Z",
                   "thumbnail_url": "https://static-cdn.jtvnw.net/previews-ttv/live_user_knuxplays-{width}x{height}.jpg"})
        })
        .collect();
    (StatusCode::OK, Json(json!({"data": data})))
}

async fn fake_twitch() -> (String, Shared) {
    let seen = Shared::default();
    let app = Router::new()
        .route("/oauth2/token", post(token))
        .route("/oauth2/revoke", post(revoke))
        .route("/helix/users", get(users))
        .route("/helix/streams", get(streams))
        .with_state(seen.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (format!("http://{addr}"), seen)
}

/// `GET /v1/twitch/streams` until `pred` holds, keeping the game's connection alive meanwhile.
async fn streams_until(stack: &Stack, session: &str, game: &mut OnlineClient, pred: impl Fn(&Value) -> bool) -> Value {
    let deadline = Instant::now() + Duration::from_secs(40);
    loop {
        game.send_game_alive();
        game.pump();
        let (st, body) = stack.get("/v1/twitch/streams", Some(session)).await;
        assert_eq!(st, 200, "{body}");
        if pred(&body) {
            return body;
        }
        assert!(Instant::now() < deadline, "streams never matched: {body}");
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_connected_player_is_live_while_playing() {
    let (twitch, seen) = fake_twitch().await;
    let stack = Stack::start(StackOptions { twitch_url: Some(twitch.clone()), ..Default::default() }).await.unwrap();
    let (session, user) = stack.create_player("knux@x.test", "Knuckles", "KNUX").await;
    let (st, me) = stack.get("/v1/me/twitch", Some(&session)).await;
    assert_eq!(st, 200);
    assert_eq!(me, json!({"available": true, "linked": null, "handle": null}));

    // Connect: the sign-in address names our client, our callback and a state.
    let (st, body) = stack.post("/v1/me/twitch/connect", Some(&session), json!({})).await;
    assert_eq!(st, 200, "{body}");
    let url = reqwest::Url::parse(body["url"].as_str().unwrap()).unwrap();
    assert_eq!(url.as_str().split('?').next().unwrap(), format!("{twitch}/oauth2/authorize"));
    let q: HashMap<String, String> = url.query_pairs().into_owned().collect();
    assert_eq!(q["client_id"], "e2e-client");
    assert_eq!(q["redirect_uri"], stack.url("/v1/twitch/callback"));
    assert_eq!(q["response_type"], "code");
    let state = q["state"].clone();

    // A refused sign-in connects nothing, and uses the state up.
    let (st, body) = stack.post("/v1/me/twitch/connect", Some(&session), json!({})).await;
    assert_eq!(st, 200);
    let refused = reqwest::Url::parse(body["url"].as_str().unwrap()).unwrap();
    let refused: HashMap<String, String> = refused.query_pairs().into_owned().collect();
    let page = |state: &str, code: &str, error: &str| {
        let url = stack.url(&format!("/v1/twitch/callback?state={state}&code={code}&error={error}"));
        let http = stack.http.clone();
        async move {
            let r = http.get(url).send().await.unwrap();
            (r.status().as_u16(), r.text().await.unwrap())
        }
    };
    let (st, text) = page(&refused["state"], "", "access_denied").await;
    assert_eq!(st, 200);
    assert!(text.contains("Not connected."), "{text}");
    assert_eq!(stack.get("/v1/me/twitch", Some(&session)).await.1["linked"], Value::Null);

    // Twitch sends the player back with the code: connected, once.
    let (st, text) = page(&state, "good", "").await;
    assert_eq!(st, 200, "{text}");
    assert!(text.contains("You're connected!"), "{text}");
    assert_eq!(page(&state, "good", "").await.0, 400, "a state works once");
    assert_eq!(seen.lock().unwrap().revoked, vec!["user".to_string()], "the user token is not kept");
    let (_, me) = stack.get("/v1/me/twitch", Some(&session)).await;
    assert_eq!(me["linked"], json!({"login": "knuxplays", "displayName": "KnuxPlays"}));
    assert_eq!(me["handle"], "knuxplays", "the profile links the account signed in to");
    let (_, profile) = stack.get(&format!("/v1/profiles/{}", user.uid), None).await;
    assert_eq!(profile["socials"][0]["handle"], "knuxplays", "{profile}");

    // Live on Twitch but not playing: not listed.
    tokio::time::sleep(Duration::from_secs(12)).await;
    let (_, body) = stack.get("/v1/twitch/streams", Some(&session)).await;
    assert_eq!(body, json!({"streams": []}));

    // In a room: listed, with the player's name and the picture's size filled in.
    let mut game = OnlineClient::connect(stack.mm_addr, &creds(&user), "0.1.0").expect("connect").0;
    game.send(&RoomRequest::Create { public: true, ruleset: None, remembered: false });
    game.wait_for(WAIT, |m| m["type"] == "room-state").expect("room");
    let body = streams_until(&stack, &session, &mut game, |b| !b["streams"].as_array().unwrap().is_empty()).await;
    let s = &body["streams"][0];
    assert_eq!(s["uid"], user.uid.as_str());
    assert_eq!(s["displayName"], "Knuckles");
    assert_eq!((s["login"].as_str(), s["twitchName"].as_str()), (Some("knuxplays"), Some("KnuxPlays")));
    assert_eq!((s["game"].as_str(), s["viewers"].as_u64()), (Some("Super Smash Bros. Melee"), Some(23)));
    assert!(s["thumbnail"].as_str().unwrap().contains("live_user_knuxplays-440x248.jpg?t="), "{s}");

    // Disconnected: off the list at once.
    let (st, _) = stack.delete("/v1/me/twitch", Some(&session)).await;
    assert_eq!(st, 204);
    assert_eq!(stack.get("/v1/twitch/streams", Some(&session)).await.1, json!({"streams": []}));
    assert_eq!(stack.get("/v1/me/twitch", Some(&session)).await.1["linked"], Value::Null);
    game.close();
    stack.shutdown().await;
}
