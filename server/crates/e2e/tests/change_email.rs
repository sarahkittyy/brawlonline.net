//! Changing the email of an account that has not verified it (the launcher's verify step,
//! "Wrong email? Change email"; bug #163).

use e2e::{Stack, StackOptions};
use serde_json::json;

const PW: &str = "correct horse battery";

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn unverified_email_can_be_changed() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (st, body) =
        s.post("/v1/auth/signup", None, json!({"email": "graham@typo.test", "password": PW, "displayName": "g"})).await;
    assert_eq!(st, 201, "{body}");
    let session = body["sessionToken"].as_str().unwrap().to_string();
    let typo_token = s.mailed_token("graham@typo.test");
    // A reset link mailed to the typo too.
    let (st, _) = s.post("/v1/auth/password-reset/request", None, json!({"email": "graham@typo.test"})).await;
    assert_eq!(st, 202);
    let typo_reset = s.mailed_token("graham@typo.test");
    assert_ne!(typo_reset, typo_token);

    // Another account's email, a bad one, the same one, no session.
    s.post("/v1/auth/signup", None, json!({"email": "other@x.test", "password": PW, "displayName": "o"})).await;
    for (email, st, code) in [
        ("Other@X.test", 409, "email_taken"),
        ("not an email", 400, "invalid_email"),
        (" GRAHAM@typo.test", 400, "same_email"),
    ] {
        let (got, body) = s.post("/v1/auth/change-email", Some(&session), json!({"email": email})).await;
        assert_eq!((got, body["error"]["code"].as_str()), (st, Some(code)), "{email}");
    }
    let (st, _) = s.post("/v1/auth/change-email", None, json!({"email": "graham@right.test"})).await;
    assert_eq!(st, 401);
    let sent = s.mailer.sent().len();

    let (st, me) = s.post("/v1/auth/change-email", Some(&session), json!({"email": " Graham@Right.test "})).await;
    assert_eq!(st, 200, "{me}");
    assert_eq!(me["email"], "graham@right.test");
    assert_eq!(me["emailVerified"], false);
    assert_eq!(s.mailer.sent().len(), sent + 1);
    let (_, me) = s.get("/v1/me", Some(&session)).await;
    assert_eq!(me["email"], "graham@right.test");

    // The links mailed to the typo no longer work: they would verify the new address,
    // or reset its password, from the typo's inbox.
    assert_eq!(s.post("/v1/auth/verify-email", None, json!({"token": typo_token})).await.0, 400);
    let (st, _) = s
        .post("/v1/auth/password-reset/confirm", None, json!({"token": typo_reset, "newPassword": "a new password!"}))
        .await;
    assert_eq!(st, 400);
    let (_, me) = s.get("/v1/me", Some(&session)).await;
    assert_eq!(me["emailVerified"], false);

    // The new address logs in and verifies; the typo is free again.
    let (st, _) = s.post("/v1/auth/login", None, json!({"email": "graham@right.test", "password": PW})).await;
    assert_eq!(st, 200);
    assert_eq!(s.post("/v1/auth/login", None, json!({"email": "graham@typo.test", "password": PW})).await.0, 401);
    let token = s.mailed_token("graham@right.test");
    assert_eq!(s.post("/v1/auth/verify-email", None, json!({"token": token})).await.0, 200);
    let (_, me) = s.get("/v1/me", Some(&session)).await;
    assert_eq!(me["emailVerified"], true);

    // A verified email is not changed here.
    let (st, body) = s.post("/v1/auth/change-email", Some(&session), json!({"email": "graham@third.test"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("already_verified")));
    let (st, _) =
        s.post("/v1/auth/signup", None, json!({"email": "graham@typo.test", "password": PW, "displayName": "t"})).await;
    assert_eq!(st, 201);
    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn email_changes_are_limited_like_resends() {
    let s = Stack::start(StackOptions { rate_limits: true, ..Default::default() }).await.unwrap();
    let (_, body) =
        s.post("/v1/auth/signup", None, json!({"email": "a@typo.test", "password": PW, "displayName": "a"})).await;
    let session = body["sessionToken"].as_str().unwrap().to_string();
    // One verification email a minute per account, shared with the resend.
    assert_eq!(s.post("/v1/auth/change-email", Some(&session), json!({"email": "a@one.test"})).await.0, 200);
    assert_eq!(s.post("/v1/auth/change-email", Some(&session), json!({"email": "a@two.test"})).await.0, 429);
    assert_eq!(s.post("/v1/auth/verify-email/resend", Some(&session), json!({})).await.0, 429);
    let (_, me) = s.get("/v1/me", Some(&session)).await;
    assert_eq!(me["email"], "a@one.test");
    s.shutdown().await;
}
