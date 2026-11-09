//! End-to-end: player profiles (docs/design/profiles.md). `GET /v1/profiles/<id>` by uid and by
//! connect code (as the page address writes it and as typed), a placing player's hidden rating,
//! banned and unknown accounts; the `/profile/<code>` web page (its link preview tags, the
//! redirects to its one address, the 404 page); and the rank beside each name in the match
//! history, as it was when the match was made.

use chrono::{Duration, Utc};
use common::ranked::PLACEMENT_SETS;
use e2e::{Stack, StackOptions, UserJson};
use serde_json::{json, Value};
use sqlx::PgPool;
use uuid::Uuid;

fn uuid(u: &UserJson) -> Uuid {
    Uuid::parse_str(&u.uid).unwrap()
}

/// Sets the player's stored standing (the profile reads `ratings`).
async fn set_rating(pool: &PgPool, uid: Uuid, rating: f64, sets: i32, rank: Option<i16>) {
    sqlx::query(
        "INSERT INTO ratings (uid, rating, sets_played, wins, losses, rank) VALUES ($1, $2, $3, $4, $5, $6)
         ON CONFLICT (uid) DO UPDATE SET rating = $2, sets_played = $3, wins = $4, losses = $5, rank = $6",
    )
    .bind(uid)
    .bind(rating)
    .bind(sets)
    .bind(sets - sets / 3)
    .bind(sets / 3)
    .bind(rank)
    .execute(pool)
    .await
    .unwrap();
}

async fn profile(stack: &Stack, id: &str) -> (u16, Value) {
    stack.get(&format!("/v1/profiles/{id}"), None).await
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn profiles_by_uid_and_code_and_their_page() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (_, a) = stack.create_player("prof-a@example.test", "Sonder<Meaning>", "SOND").await;
    let (_, b) = stack.create_player("prof-b@example.test", "newbie", "NEWB").await;
    let (_, c) = stack.create_player("prof-c@example.test", "banned one", "BANN").await;
    let placed = PLACEMENT_SETS as i32;
    // a: Gold 2 (rank index 7), placed; b: placing, 2 sets.
    set_rating(&stack.pool, uuid(&a), 1523.44, placed + 4, Some(7)).await;
    set_rating(&stack.pool, uuid(&b), 1460.0, 2, None).await;
    sqlx::query("UPDATE users SET banned_until = now() + interval '1 day' WHERE uid = $1")
        .bind(uuid(&c))
        .execute(&stack.pool)
        .await
        .unwrap();
    let slug = a.connect_code.replace('#', "-").to_lowercase();

    // The same profile by uid, by the page's address, by the code as typed (any case, '#' encoded).
    let (st, by_uid) = profile(&stack, &a.uid).await;
    assert_eq!(st, 200, "{by_uid}");
    for id in [slug.clone(), slug.to_uppercase(), a.connect_code.replace('#', "%23").to_lowercase()] {
        let (st, p) = profile(&stack, &id).await;
        assert_eq!((st, &p), (200, &by_uid), "{id}");
    }
    assert_eq!(by_uid["displayName"], "Sonder<Meaning>");
    assert_eq!(by_uid["connectCode"], a.connect_code);
    assert_eq!(by_uid["link"].as_str().unwrap(), stack.url(&format!("/profile/{slug}")));
    let r = &by_uid["ranked"];
    assert_eq!(r["rank"], json!({"tier": "gold", "division": 2}));
    assert!((r["rating"].as_f64().unwrap() - 1523.44).abs() < 1e-9);
    assert_eq!((r["setsPlayed"].as_u64(), r["setsToPlace"].as_u64()), (Some(placed as u64 + 4), Some(0)));
    assert_eq!((r["wins"].as_u64(), r["losses"].as_u64()), (Some(6), Some(3)));
    assert_eq!((r["position"].as_u64(), r["rankedPlayers"].as_u64()), (Some(1), Some(1)));

    // Placing: no rating, rank or position.
    let (st, p) = profile(&stack, &b.uid).await;
    assert_eq!(st, 200);
    let r = &p["ranked"];
    assert!(r["rating"].is_null() && r["rank"].is_null() && r["position"].is_null(), "{p}");
    assert_eq!(r["setsToPlace"].as_u64(), Some(PLACEMENT_SETS as u64 - 2));

    // Banned, unknown, and not an address at all.
    let banned_slug = c.connect_code.replace('#', "-");
    for id in [c.uid.as_str(), banned_slug.as_str(), "zzzz-9999", &Uuid::new_v4().to_string(), "not a code"] {
        assert_eq!(profile(&stack, id).await.0, 404, "{id}");
    }

    // The web page, without following redirects.
    let http = reqwest::Client::builder().redirect(reqwest::redirect::Policy::none()).build().unwrap();
    let resp = http.get(stack.url(&format!("/profile/{slug}"))).send().await.unwrap();
    assert_eq!(resp.status().as_u16(), 200);
    assert!(resp.headers()["content-type"].to_str().unwrap().starts_with("text/html"));
    assert!(resp.headers()["content-security-policy"].to_str().unwrap().contains("default-src 'none'"));
    let html = resp.text().await.unwrap();
    assert!(html.contains("<h1 class=\"name\">Sonder&lt;Meaning&gt;</h1>"), "{html}");
    assert!(
        html.contains(&format!("<meta property=\"og:title\" content=\"Sonder&lt;Meaning&gt; ({})\">", a.connect_code))
    );
    assert!(html.contains("content=\"Gold 2 · Rating 1523.4 · #1 of 1 · 6W-3L\""), "{html}");
    assert!(html.contains("/assets/ranks/gold-2.png"));
    // Other spellings of the address go to the one address; unknown and banned ones are 404 pages.
    for other in [slug.to_uppercase(), a.uid.clone(), a.connect_code.replace('#', "-0")] {
        let resp = http.get(stack.url(&format!("/profile/{other}"))).send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 308, "{other}");
        assert_eq!(resp.headers()["location"], format!("/profile/{slug}").as_str(), "{other}");
    }
    for missing in ["zzzz-9999", banned_slug.to_lowercase().as_str(), "x"] {
        let resp = http.get(stack.url(&format!("/profile/{missing}"))).send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 404, "{missing}");
        assert!(resp.text().await.unwrap().contains("noindex"));
    }
    stack.shutdown().await;
}

/// A rating change for `uid` at `at`, leaving them at `rank_after` (a `common::ranked::Rank`
/// index; None while placing).
async fn rating_event(pool: &PgPool, uid: Uuid, at: chrono::DateTime<Utc>, rank_after: Option<i16>) {
    let id = format!("prof-{}", Uuid::new_v4());
    sqlx::query("INSERT INTO mm_matches (match_id, mode, players, is_host, status) VALUES ($1, 0, $2, $3, 'COMPLETE')")
        .bind(&id)
        .bind(vec![uid])
        .bind(uid)
        .execute(pool)
        .await
        .unwrap();
    sqlx::query(
        "INSERT INTO rating_events (uid, match_id, score, rating_before, rating_after, opponent_rating, k,
                                    sets_before, created_at, rank_after)
         VALUES ($1, $2, 1, 1400, 1450, 1400, 32, 10, $3, $4)",
    )
    .bind(uid)
    .bind(&id)
    .bind(at)
    .bind(rank_after)
    .execute(pool)
    .await
    .unwrap();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn match_history_shows_each_players_rank_then() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (sa, a) = stack.create_player("hist-a@example.test", "alpha", "ALPH").await;
    let (_, b) = stack.create_player("hist-b@example.test", "bravo", "BRAV").await;
    let t0 = Utc::now() - Duration::hours(3);
    // a: Silver 1 two hours before the first match, Diamond 3 between the two; b never ranked.
    rating_event(&stack.pool, uuid(&a), t0 - Duration::hours(2), Some(5)).await;
    rating_event(&stack.pool, uuid(&a), t0 + Duration::minutes(30), Some(12)).await;
    let mut ids = vec![];
    for minutes in [0, 60] {
        let id = format!("mode.unranked-prof-{minutes}-{}", Uuid::new_v4().simple());
        let p = [uuid(&a), uuid(&b)];
        common::db::insert_match(&stack.pool, &id, 1, &p, p[0], &[], "other", None).await.unwrap();
        sqlx::query("UPDATE mm_matches SET created_at = $2 WHERE match_id = $1")
            .bind(&id)
            .bind(t0 + Duration::minutes(minutes))
            .execute(&stack.pool)
            .await
            .unwrap();
        let (st, body) = stack
            .post(
                "/v1/ranked/report-game",
                None,
                json!({
                    "uid": a.uid, "playKey": a.play_key, "matchId": id, "gameIndex": 1,
                    "winner": a.uid, "stageId": 31, "durationFrames": 9000,
                    "players": [{"uid": a.uid, "characterId": 9, "stocksRemaining": 2, "damage": 40.5}],
                }),
            )
            .await;
        assert_eq!(st, 200, "{body}");
        ids.push(id);
    }
    let (st, page) = stack.get("/v1/me/matches?mode=unranked", Some(&sa)).await;
    assert_eq!(st, 200, "{page}");
    let rank_of = |id: &str, u: &UserJson| -> Value {
        let m = page["matches"].as_array().unwrap().iter().find(|m| m["matchId"] == id).unwrap();
        m["players"].as_array().unwrap().iter().find(|p| p["uid"] == u.uid.as_str()).unwrap()["rank"].clone()
    };
    assert_eq!(rank_of(&ids[0], &a), json!({"tier": "silver", "division": 1}));
    assert_eq!(rank_of(&ids[1], &a), json!({"tier": "diamond", "division": 3}));
    assert!(rank_of(&ids[0], &b).is_null() && rank_of(&ids[1], &b).is_null());
    stack.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn players_set_their_mains_country_and_socials() {
    let stack = Stack::start(StackOptions::default()).await.unwrap();
    let (session, a) = stack.create_player("edit-a@example.test", "editor", "EDIT").await;
    // Nothing set: empty lists, no country.
    let (st, p) = profile(&stack, &a.uid).await;
    assert_eq!(st, 200);
    assert_eq!((&p["mains"], &p["country"], &p["socials"]), (&json!([]), &Value::Null, &json!([])));

    // Everything at once: mains in order (the primary first), a country in any case, socials as
    // usernames, @names and links.
    let body = json!({
        "mains": ["marth", "random", "sheik"],
        "country": "GB-ENG",
        "socials": {"twitch": "https://www.twitch.tv/SarahKitty/", "discord": "SarahKittyy", "youtube": "@sarah"},
    });
    let (st, p) = stack.put("/v1/me/profile", Some(&session), body).await;
    assert_eq!(st, 200, "{p}");
    assert_eq!(p["mains"], json!(["marth", "random", "sheik"]));
    assert_eq!(p["country"], "gb-eng");
    assert_eq!(
        p["socials"],
        json!([
            {"platform": "discord", "handle": "sarahkittyy", "display": "sarahkittyy", "url": null},
            {"platform": "twitch", "handle": "sarahkitty", "display": "@sarahkitty", "url": "https://twitch.tv/sarahkitty"},
            {"platform": "youtube", "handle": "@sarah", "display": "@sarah", "url": "https://youtube.com/@sarah"},
        ])
    );
    assert_eq!(profile(&stack, &a.uid).await.1, p, "the public profile has the same");

    // A field left out stays; null clears the country; a blank social removes it.
    let (st, p) = stack.put("/v1/me/profile", Some(&session), json!({"mains": ["fox"]})).await;
    assert_eq!(st, 200, "{p}");
    assert_eq!((p["mains"].clone(), p["country"].clone()), (json!(["fox"]), json!("gb-eng")));
    assert_eq!(p["socials"].as_array().unwrap().len(), 3);
    let (st, p) = stack
        .put("/v1/me/profile", Some(&session), json!({"country": null, "socials": {"discord": "", "youtube": null}}))
        .await;
    assert_eq!(st, 200, "{p}");
    assert!(p["country"].is_null());
    assert_eq!(
        p["socials"].as_array().unwrap().iter().map(|s| s["platform"].clone()).collect::<Vec<_>>(),
        [json!("twitch")]
    );

    // Refused, with a message for the player, and nothing changed.
    for bad in [
        json!({"mains": ["marth", "giga_bowser"]}),
        json!({"mains": ["marth", "marth"]}),
        json!({"country": "zz"}),
        json!({"socials": {"twitch": "https://evil.example/x"}}),
        json!({"socials": {"myspace": "tom"}}),
    ] {
        let (st, body) = stack.put("/v1/me/profile", Some(&session), bad.clone()).await;
        assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_profile")), "{bad}: {body}");
        assert!(!body["error"]["message"].as_str().unwrap().is_empty());
    }
    let too_many: Vec<&str> = common::profile::CHARACTERS.iter().map(|(k, _)| *k).collect();
    assert_eq!(stack.put("/v1/me/profile", Some(&session), json!({"mains": too_many})).await.0, 400);
    assert_eq!(profile(&stack, &a.uid).await.1["mains"], json!(["fox"]));
    assert_eq!(stack.put("/v1/me/profile", None, json!({"mains": []})).await.0, 401);

    // The web page shows them.
    let slug = a.connect_code.replace('#', "-").to_lowercase();
    let html = stack.http.get(stack.url(&format!("/profile/{slug}"))).send().await.unwrap().text().await.unwrap();
    assert!(html.contains("<li class=\"main primary\" title=\"Primary\">Fox</li>"), "{html}");
    assert!(html.contains("href=\"https://twitch.tv/sarahkitty\""));
    stack.shutdown().await;
}
