//! End-to-end: rulesets (docs/rulesets-protocol.md). Publishing (codes, versions, forks,
//! "unchanged"), public lookups and "newer version" links, hiding, the database refusing to
//! change or delete a published version, a deleted publisher, drafts, the remembered ruleset and
//! the recent list, who may publish, and the publish rate limit.

use common::rules::{parse_ruleset_code, Ruleset};
use e2e::{Stack, StackOptions};
use serde_json::{json, Value};

const PW: &str = "correct horse battery";

/// A Bo3 with 1-2-1 strikes, 2 bans and DSR, as JSON.
fn weekly() -> Value {
    json!({
        "format": 1, "name": "NorCal Weekly",
        "rules": {"stocks": 3, "timeMinutes": 7, "autoLCancel": true},
        "stages": {"starters": [1, 2, 33, 45, 46], "counterpicks": [3, 31]},
        "bestOf": 3, "strikes": [1, 2, 1], "bans": 2, "dsr": true
    })
}

fn code_of(v: &Value) -> String {
    v["code"].as_str().unwrap().to_string()
}

async fn publish(s: &Stack, token: &str, body: Value) -> Value {
    let (st, v) = s.post("/v1/rulesets", Some(token), body).await;
    assert_eq!(st, 201, "{v}");
    v
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn publish_lookup_versions_and_hiding() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (alice, alice_json) = s.create_player("alice@x.test", "alice", "ALIC").await;
    let (bob, _) = s.create_player("bob@x.test", "bob", "BOB").await;

    // Version 1: a fresh code, the publisher's name, the share link, the hash of what came back.
    let v1 = publish(&s, &alice, json!({"ruleset": weekly()})).await;
    let c1 = code_of(&v1);
    assert_eq!(parse_ruleset_code(&c1).as_deref(), Some(c1.as_str()));
    assert_eq!(v1["ruleset"]["version"], 1);
    assert_eq!(v1["ruleset"]["by"], "alice");
    assert_eq!(v1["ruleset"]["rules"]["stocks"], 3);
    assert_eq!(v1["ruleset"]["rules"]["damageRatio"], 1.0, "defaults filled in");
    assert_eq!(v1["link"], format!("{}/r/{c1}", s.base));
    assert_eq!(v1["hidden"], false);
    assert!(v1["newer"].is_null());
    let r1: Ruleset = serde_json::from_value(v1["ruleset"].clone()).unwrap();
    assert_eq!(v1["hash"], r1.hash());
    // The database holds exactly the hashed bytes.
    let (json_text, hash, owner): (String, String, Option<uuid::Uuid>) =
        sqlx::query_as("SELECT json, hash, owner FROM rulesets WHERE code = $1")
            .bind(&c1)
            .fetch_one(&s.pool)
            .await
            .unwrap();
    assert_eq!(json_text, r1.canonical_json());
    assert_eq!(hash, r1.hash());
    assert_eq!(owner.unwrap().to_string(), alice_json.uid);

    // Anyone can look it up, any case; the owner-only field is left out.
    let (st, got) = s.get(&format!("/v1/rulesets/{}", c1.to_lowercase()), None).await;
    assert_eq!(st, 200, "{got}");
    assert_eq!(got["ruleset"], v1["ruleset"]);
    assert_eq!(got["hash"], v1["hash"]);
    assert!(got.get("hidden").is_none(), "{got}");
    for bad in ["BCDFG", "ABCDEF", "BCDFGHJ", "%20"] {
        let (st, body) = s.get(&format!("/v1/rulesets/{bad}"), None).await;
        assert_eq!((st, body["error"]["code"].as_str()), (404, Some("unknown_ruleset")), "{bad}");
    }
    let unknown = if c1 == "BBBBBB" { "CCCCCC" } else { "BBBBBB" };
    let (st, _) = s.get(&format!("/v1/rulesets/{unknown}"), None).await;
    assert_eq!(st, 404);

    // Republishing it unchanged (the version and publisher don't count) is refused.
    let mut same = v1["ruleset"].clone();
    same["by"] = json!("someone");
    same["version"] = json!(7);
    let (st, body) = s.post("/v1/rulesets", Some(&alice), json!({"ruleset": same, "basedOn": c1})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("unchanged")), "{body}");

    // An edit is the next version, with a new code; the old one links to it.
    let mut edited = weekly();
    edited["bestOf"] = json!(5);
    let v2 = publish(&s, &alice, json!({"ruleset": edited, "basedOn": c1.to_lowercase()})).await;
    let c2 = code_of(&v2);
    assert_ne!(c1, c2);
    assert_eq!(v2["ruleset"]["version"], 2);
    let (_, got) = s.get(&format!("/v1/rulesets/{c1}"), None).await;
    assert_eq!(got["newer"], json!({"code": c2, "version": 2}));
    assert_eq!(got["ruleset"]["bestOf"], 3, "version 1 is unchanged");

    // Someone else's edit is version 1 of their own ruleset.
    let mut forked = weekly();
    forked["bans"] = json!(1);
    let b1 = publish(&s, &bob, json!({"ruleset": forked, "basedOn": c2})).await;
    assert_eq!((b1["ruleset"]["version"].as_u64(), b1["ruleset"]["by"].as_str()), (Some(1), Some("bob")));
    let (_, got) = s.get(&format!("/v1/rulesets/{c2}"), None).await;
    assert!(got["newer"].is_null());

    // Bad rulesets and bad requests.
    let mut bad = weekly();
    bad["strikes"] = json!([1, 1]);
    let (st, body) = s.post("/v1/rulesets", Some(&alice), json!({"ruleset": bad})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_ruleset")), "{body}");
    assert!(body["error"]["message"].as_str().unwrap().contains("exactly one"));
    let mut unknown_field = weekly();
    unknown_field["bestof"] = json!(3);
    let (st, _) = s.post("/v1/rulesets", Some(&alice), json!({"ruleset": unknown_field})).await;
    assert!((400..500).contains(&st), "{st}");
    let (st, _) = s.post("/v1/rulesets", Some(&alice), json!({})).await;
    assert_eq!(st, 400);
    let (st, body) = s.post("/v1/rulesets", Some(&alice), json!({"ruleset": weekly(), "basedOn": unknown})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (404, Some("unknown_ruleset")));
    let (st, _) = s.post("/v1/rulesets", None, json!({"ruleset": weekly()})).await;
    assert_eq!(st, 401);
    // Unused set fields are cleared, not refused: a no-set ruleset with bans publishes without.
    let mut loose = weekly();
    loose["bestOf"] = json!(0);
    loose["name"] = json!("  Loose  ");
    let l = publish(&s, &alice, json!({"ruleset": loose})).await;
    assert_eq!((l["ruleset"]["bans"].as_u64(), l["ruleset"]["dsr"].as_bool()), (Some(0), Some(false)));
    assert_eq!(l["ruleset"]["strikes"], json!([]));
    assert_eq!(l["ruleset"]["name"], "Loose");

    // Hiding: off the owner's newer-version links; the code keeps working; only the owner can.
    let (st, _) = s.put(&format!("/v1/me/rulesets/{c2}/hidden"), Some(&alice), json!({"hidden": true})).await;
    assert_eq!(st, 204);
    let (_, got) = s.get(&format!("/v1/rulesets/{c1}"), None).await;
    assert!(got["newer"].is_null(), "{got}");
    let (st, got) = s.get(&format!("/v1/rulesets/{c2}"), None).await;
    assert_eq!((st, got["ruleset"]["version"].as_u64()), (200, Some(2)));
    let (st, _) = s.put(&format!("/v1/me/rulesets/{c1}/hidden"), Some(&bob), json!({"hidden": true})).await;
    assert_eq!(st, 404);
    let (st, mine) = s.get("/v1/me/rulesets", Some(&alice)).await;
    assert_eq!(st, 200, "{mine}");
    let published = mine["published"].as_array().unwrap();
    assert_eq!(published.len(), 3);
    assert_eq!(published[0]["code"], code_of(&l), "newest first");
    let p2 = published.iter().find(|p| p["code"] == c2.as_str()).unwrap();
    assert_eq!(p2["hidden"], true);
    let p1 = published.iter().find(|p| p["code"] == c1.as_str()).unwrap();
    assert_eq!(p1["hidden"], false);
    assert!(mine["drafts"].as_array().unwrap().is_empty());
    // Shown again.
    let (st, _) = s.put(&format!("/v1/me/rulesets/{c2}/hidden"), Some(&alice), json!({"hidden": false})).await;
    assert_eq!(st, 204);
    let (_, got) = s.get(&format!("/v1/rulesets/{c1}"), None).await;
    assert_eq!(got["newer"]["code"], c2.as_str());

    // The database itself refuses to change or delete a published version.
    for sql in [
        "UPDATE rulesets SET name = 'changed' WHERE code = $1",
        "UPDATE rulesets SET json = '{}' WHERE code = $1",
        "UPDATE rulesets SET owner = gen_random_uuid() WHERE code = $1",
        "DELETE FROM rulesets WHERE code = $1",
    ] {
        let r = sqlx::query(sql).bind(&c1).execute(&s.pool).await;
        assert!(r.is_err(), "{sql} went through");
    }

    // A deleted publisher: the rulesets stay, with the name they had.
    let alice_uid = uuid::Uuid::parse_str(&alice_json.uid).unwrap();
    sqlx::query("UPDATE users SET display_name = 'alice2' WHERE uid = $1")
        .bind(alice_uid)
        .execute(&s.pool)
        .await
        .unwrap();
    let (_, got) = s.get(&format!("/v1/rulesets/{c1}"), None).await;
    assert_eq!(got["ruleset"]["by"], "alice2", "follows renames");
    assert_eq!(got["hash"], v1["hash"], "the name is not hashed");
    assert!(accounts::store::delete_user(&s.pool, alice_uid).await.unwrap());
    let (st, got) = s.get(&format!("/v1/rulesets/{c1}"), None).await;
    assert_eq!(st, 200);
    assert_eq!(got["ruleset"]["by"], "alice");
    assert_eq!(got["newer"]["code"], c2.as_str());

    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn drafts() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (alice, _) = s.create_player("alice@x.test", "alice", "ALIC").await;
    let (bob, _) = s.create_player("bob@x.test", "bob", "BOB").await;

    // A draft may not validate yet: it says why.
    let mut wip = weekly();
    wip["strikes"] = json!([1, 2]);
    let (st, d) = s.post("/v1/me/ruleset-drafts", Some(&alice), json!({"ruleset": wip})).await;
    assert_eq!(st, 201, "{d}");
    let id = d["id"].as_str().unwrap().to_string();
    assert!(d["problem"].as_str().unwrap().contains("exactly one"), "{d}");
    assert!(d["basedOn"].is_null());
    let (st, body) = s.post("/v1/rulesets", Some(&alice), json!({"draftId": id})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("invalid_ruleset")));

    // Only the owner sees or changes it.
    for (st, _) in [
        s.get(&format!("/v1/me/ruleset-drafts/{id}"), Some(&bob)).await,
        s.put(&format!("/v1/me/ruleset-drafts/{id}"), Some(&bob), json!({"ruleset": weekly()})).await,
        s.delete(&format!("/v1/me/ruleset-drafts/{id}"), Some(&bob)).await,
        s.post("/v1/rulesets", Some(&bob), json!({"draftId": id})).await,
    ] {
        assert_eq!(st, 404);
    }

    // Fixed, listed, published: the draft is gone and the ruleset is out.
    let (st, d) = s.put(&format!("/v1/me/ruleset-drafts/{id}"), Some(&alice), json!({"ruleset": weekly()})).await;
    assert_eq!(st, 200, "{d}");
    assert!(d["problem"].is_null(), "{d}");
    let (_, mine) = s.get("/v1/me/rulesets", Some(&alice)).await;
    assert_eq!(mine["drafts"].as_array().unwrap().len(), 1);
    assert_eq!(mine["drafts"][0]["ruleset"]["name"], "NorCal Weekly");
    let v1 = publish(&s, &alice, json!({"draftId": id})).await;
    let c1 = code_of(&v1);
    let (st, _) = s.get(&format!("/v1/me/ruleset-drafts/{id}"), Some(&alice)).await;
    assert_eq!(st, 404);

    // A draft based on a published version publishes the next version.
    let mut edit = v1["ruleset"].clone();
    edit["rules"]["stocks"] = json!(4);
    let (st, d) =
        s.post("/v1/me/ruleset-drafts", Some(&alice), json!({"ruleset": edit, "basedOn": c1.to_lowercase()})).await;
    assert_eq!(st, 201, "{d}");
    assert_eq!(d["basedOn"], c1.as_str());
    assert_eq!(d["ruleset"]["version"], 0, "a draft has no version");
    assert_eq!(d["ruleset"]["by"], "");
    let v2 = publish(&s, &alice, json!({"draftId": d["id"]})).await;
    assert_eq!(v2["ruleset"]["version"], 2);
    // Based on a code nobody published.
    let unknown = if c1 == "BBBBBB" { "CCCCCC" } else { "BBBBBB" };
    let (st, _) = s.post("/v1/me/ruleset-drafts", Some(&alice), json!({"ruleset": weekly(), "basedOn": unknown})).await;
    assert_eq!(st, 404);

    // Deleting, and the cap on drafts.
    let (_, d) = s.post("/v1/me/ruleset-drafts", Some(&alice), json!({"ruleset": weekly()})).await;
    let (st, _) = s.delete(&format!("/v1/me/ruleset-drafts/{}", d["id"].as_str().unwrap()), Some(&alice)).await;
    assert_eq!(st, 204);
    for n in 0..accounts::rulesets::MAX_DRAFTS {
        let mut r = weekly();
        r["name"] = json!(format!("draft {n}"));
        let (st, body) = s.post("/v1/me/ruleset-drafts", Some(&bob), json!({"ruleset": r})).await;
        assert_eq!(st, 201, "{body}");
    }
    let (st, body) = s.post("/v1/me/ruleset-drafts", Some(&bob), json!({"ruleset": weekly()})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("too_many_drafts")));
    let (_, mine) = s.get("/v1/me/rulesets", Some(&bob)).await;
    assert_eq!(mine["drafts"].as_array().unwrap().len(), 50);
    assert_eq!(mine["drafts"][0]["ruleset"]["name"], "draft 49", "newest edit first");

    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn remembered_ruleset_and_recent_list() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (alice, _) = s.create_player("alice@x.test", "alice", "ALIC").await;
    let (bob, bob_json) = s.create_player("bob@x.test", "bob", "BOB").await;

    let mut codes = vec![];
    for n in 0..12 {
        let mut r = weekly();
        r["name"] = json!(format!("Weekly {n}"));
        codes.push(code_of(&publish(&s, &alice, json!({"ruleset": r})).await));
    }

    // P+ default to start with.
    let (st, got) = s.get("/v1/me/ruleset", Some(&bob)).await;
    assert_eq!((st, got.clone()), (200, json!({"ruleset": null})));
    let (st, got) = s.get("/v1/me/rulesets/recent", Some(&bob)).await;
    assert_eq!((st, got), (200, json!({"rulesets": []})));

    // Someone else's ruleset, typed in any case.
    let (st, got) = s.put("/v1/me/ruleset", Some(&bob), json!({"code": codes[0].to_lowercase()})).await;
    assert_eq!(st, 200, "{got}");
    assert_eq!(got["ruleset"]["code"], codes[0].as_str());
    assert_eq!(got["ruleset"]["ruleset"]["by"], "alice");
    assert!(got["ruleset"].get("hidden").is_none());
    let (_, again) = s.get("/v1/me/ruleset", Some(&bob)).await;
    assert_eq!(again, got);
    // mm reads the same (common::db, for in-game Create Room).
    let bob_uid = uuid::Uuid::parse_str(&bob_json.uid).unwrap();
    let p = common::db::remembered_ruleset(&s.pool, bob_uid).await.unwrap().unwrap();
    assert_eq!((p.code.as_str(), p.ruleset.name.as_str(), p.ruleset.version), (codes[0].as_str(), "Weekly 0", 1));

    // Unknown codes leave it as it was.
    for bad in ["BCDFG", "AAAAAA"] {
        let (st, body) = s.put("/v1/me/ruleset", Some(&bob), json!({"code": bad})).await;
        assert_eq!((st, body["error"]["code"].as_str()), (404, Some("unknown_ruleset")), "{bad}");
    }
    let (_, still) = s.get("/v1/me/ruleset", Some(&bob)).await;
    assert_eq!(still["ruleset"]["code"], codes[0].as_str());

    // Every code made the remembered one goes to the top of the recent list; 10 are kept, and
    // using one again moves it up rather than adding it twice.
    for c in &codes {
        let (st, _) = s.put("/v1/me/ruleset", Some(&bob), json!({"code": c})).await;
        assert_eq!(st, 200);
    }
    let (st, _) = s.put("/v1/me/ruleset", Some(&bob), json!({"code": codes[5]})).await;
    assert_eq!(st, 200);
    let (_, recent) = s.get("/v1/me/rulesets/recent", Some(&bob)).await;
    let got: Vec<&str> = recent["rulesets"].as_array().unwrap().iter().map(|r| r["code"].as_str().unwrap()).collect();
    let mut want: Vec<&str> = codes[2..].iter().rev().map(String::as_str).filter(|c| *c != codes[5]).collect();
    want.insert(0, &codes[5]);
    assert_eq!(got, want);
    assert!(recent["rulesets"][0]["usedAt"].is_string());
    assert_eq!(recent["rulesets"][0]["ruleset"]["name"], "Weekly 5");

    // X: back to P+ default; the recent list stays.
    let (st, got) = s.put("/v1/me/ruleset", Some(&bob), json!({"code": null})).await;
    assert_eq!((st, got), (200, json!({"ruleset": null})));
    let (_, recent) = s.get("/v1/me/rulesets/recent", Some(&bob)).await;
    assert_eq!(recent["rulesets"].as_array().unwrap().len(), 10);
    // Logged out: nothing.
    let (st, _) = s.get("/v1/me/ruleset", None).await;
    assert_eq!(st, 401);

    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn who_may_publish_and_the_rate_limit() {
    let s = Stack::start(StackOptions { rate_limits: true, ..Default::default() }).await.unwrap();

    // Signed up, email not verified: no.
    let (st, body) =
        s.post("/v1/auth/signup", None, json!({"email": "new@x.test", "password": PW, "displayName": "newbie"})).await;
    assert_eq!(st, 201, "{body}");
    let newbie = body["sessionToken"].as_str().unwrap().to_string();
    let (st, body) = s.post("/v1/rulesets", Some(&newbie), json!({"ruleset": weekly()})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("email_not_verified")));
    // Verified, no connect code yet: still no.
    let token = s.mailed_token("new@x.test");
    let (st, _) = s.post("/v1/auth/verify-email", None, json!({"token": token})).await;
    assert_eq!(st, 200);
    let (st, body) = s.post("/v1/rulesets", Some(&newbie), json!({"ruleset": weekly()})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("not_activated")));
    // Drafts are fine before that.
    let (st, _) = s.post("/v1/me/ruleset-drafts", Some(&newbie), json!({"ruleset": weekly()})).await;
    assert_eq!(st, 201);

    // 10 publishes an hour.
    let (alice, _) = s.create_player("alice@x.test", "alice", "ALIC").await;
    for n in 0..10 {
        let mut r = weekly();
        r["name"] = json!(format!("Weekly {n}"));
        publish(&s, &alice, json!({"ruleset": r})).await;
    }
    let mut r = weekly();
    r["name"] = json!("one too many");
    let (st, body) = s.post("/v1/rulesets", Some(&alice), json!({"ruleset": r})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (429, Some("rate_limited")), "{body}");
    // A refused (invalid or unchanged) publish doesn't use one up: checked before the limit.
    let (alice2, _) = s.create_player("alice2@x.test", "alice2", "ALII").await;
    let mut bad = weekly();
    bad["bestOf"] = json!(4);
    for _ in 0..12 {
        let (st, _) = s.post("/v1/rulesets", Some(&alice2), json!({"ruleset": bad})).await;
        assert_eq!(st, 400);
    }
    publish(&s, &alice2, json!({"ruleset": weekly()})).await;

    s.shutdown().await;
}

/// The codes of a public list page, in order.
fn codes(page: &Value) -> Vec<String> {
    page["rulesets"].as_array().unwrap().iter().map(code_of).collect()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn public_list_and_search() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (alice, _) = s.create_player("alice@x.test", "alice", "ALIC").await;
    let (bob, _) = s.create_player("bob@x.test", "bobby", "BOB").await;

    // alice: two versions of one ruleset; bob: a name with LIKE's wildcards; alice: one she hides.
    let a1 = publish(&s, &alice, json!({"ruleset": weekly()})).await;
    let mut edited = weekly();
    edited["bestOf"] = json!(5);
    let a2 = publish(&s, &alice, json!({"ruleset": edited, "basedOn": code_of(&a1)})).await;
    let mut ffa = weekly();
    ffa["name"] = json!("100% FFA_night");
    ffa["bestOf"] = json!(0);
    let b1 = publish(&s, &bob, json!({"ruleset": ffa})).await;
    let mut secret = weekly();
    secret["name"] = json!("Secret");
    let h1 = publish(&s, &alice, json!({"ruleset": secret})).await;
    let (st, _) =
        s.put(&format!("/v1/me/rulesets/{}/hidden", code_of(&h1)), Some(&alice), json!({"hidden": true})).await;
    assert_eq!(st, 204);

    // For anyone: each ruleset's newest visible version, newest first; hidden ones left out.
    let (st, page) = s.get("/v1/rulesets", None).await;
    assert_eq!(st, 200, "{page}");
    assert_eq!(codes(&page), vec![code_of(&b1), code_of(&a2)]);
    assert!(page["next"].is_null());
    let first = &page["rulesets"][1];
    assert_eq!((first["ruleset"]["by"].as_str(), first["ruleset"]["version"].as_u64()), (Some("alice"), Some(2)));
    assert!(first["newer"].is_null() && first.get("hidden").is_none(), "{first}");
    assert_eq!(first["link"], format!("{}/r/{}", s.base, code_of(&a2)));

    // Search: the name, the publisher, the code, in any case; LIKE's wildcards are plain text.
    for (q, want) in [
        ("norcal", vec![code_of(&a2)]),
        ("WEEKLY", vec![code_of(&a2)]),
        ("bobby", vec![code_of(&b1)]),
        ("ALI", vec![code_of(&a2)]),
        ("%", vec![code_of(&b1)]),
        ("_", vec![code_of(&b1)]),
        ("secret", vec![]),
        ("nothing like it", vec![]),
    ] {
        let (st, page) = s.get(&format!("/v1/rulesets?q={}", urlencode(q)), None).await;
        assert_eq!(st, 200, "{q}: {page}");
        assert_eq!(codes(&page), want, "{q}");
    }
    let (_, page) = s.get(&format!("/v1/rulesets?q={}", code_of(&a2).to_lowercase()), None).await;
    assert_eq!(codes(&page), vec![code_of(&a2)]);
    // The older version's code is not in the list (its newer version is).
    let (_, page) = s.get(&format!("/v1/rulesets?q={}", code_of(&a1)), None).await;
    assert!(codes(&page).is_empty());

    // Pages: one at a time, following `next`.
    let mut seen = vec![];
    let mut q = "/v1/rulesets?limit=1".to_string();
    loop {
        let (st, page) = s.get(&q, None).await;
        assert_eq!(st, 200, "{page}");
        seen.extend(codes(&page));
        match page["next"].as_str() {
            Some(c) => q = format!("/v1/rulesets?limit=1&after={c}"),
            None => break,
        }
        assert!(seen.len() < 10);
    }
    assert_eq!(seen, vec![code_of(&b1), code_of(&a2)]);
    let (st, body) = s.get("/v1/rulesets?after=bogus", None).await;
    assert_eq!((st, body["error"]["code"].as_str()), (400, Some("bad_cursor")));

    // Hiding the newest version shows the one before it; showing it again brings it back.
    let (st, _) =
        s.put(&format!("/v1/me/rulesets/{}/hidden", code_of(&a2)), Some(&alice), json!({"hidden": true})).await;
    assert_eq!(st, 204);
    let (_, page) = s.get("/v1/rulesets", None).await;
    assert_eq!(codes(&page), vec![code_of(&b1), code_of(&a1)]);
    let (st, _) =
        s.put(&format!("/v1/me/rulesets/{}/hidden", code_of(&a2)), Some(&alice), json!({"hidden": false})).await;
    assert_eq!(st, 204);
    let (_, page) = s.get("/v1/rulesets", None).await;
    assert_eq!(codes(&page), vec![code_of(&b1), code_of(&a2)]);

    s.shutdown().await;
}

fn urlencode(s: &str) -> String {
    s.bytes().map(|b| if b.is_ascii_alphanumeric() { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}
