//! The accounts HTTP API against a real Postgres.

use e2e::{Stack, StackOptions};
use serde_json::json;

const PW: &str = "correct horse battery";

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn signup_verification_and_code_assignment() {
    let s = Stack::start(StackOptions::default()).await.unwrap();

    // Validation.
    for (body, code) in [
        (json!({"email": "bad", "password": PW, "displayName": "x"}), "invalid_email"),
        (json!({"email": "a@x.test", "password": "short", "displayName": "x"}), "invalid_password"),
        (json!({"email": "a@x.test", "password": PW, "displayName": ""}), "invalid_display_name"),
        (json!({"email": "a@x.test", "password": PW, "displayName": "sixteen chars!!!"}), "invalid_display_name"),
        (json!({"email": "a@x.test", "password": PW, "displayName": "back\\slash"}), "invalid_display_name"),
    ] {
        let (st, resp) = s.post("/v1/auth/signup", None, body).await;
        assert_eq!((st, resp["error"]["code"].as_str()), (400, Some(code)));
    }
    assert!(s.mailer.sent().is_empty());

    // Sign-up is open: no invite code.
    let (st, body) = s
        .post("/v1/auth/signup", None, json!({"email": " Sarah@X.test ", "password": PW, "displayName": "sarah"}))
        .await;
    assert_eq!(st, 201, "{body}");
    let session = body["sessionToken"].as_str().unwrap().to_string();
    assert_eq!(body["user"]["email"], "sarah@x.test");
    assert_eq!(body["user"]["emailVerified"], false);
    assert_eq!(body["user"]["emailVerificationRequired"], true);
    assert!(body["user"]["playKey"].is_null());
    assert!(body["user"]["userJson"].is_null());

    // Emails are unique, and a taken email sends nothing.
    let (st, body) =
        s.post("/v1/auth/signup", None, json!({"email": "SARAH@x.test", "password": PW, "displayName": "o"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("email_taken")));
    assert_eq!(s.mailer.sent().len(), 1);
    // An older launcher that still sends an invite code field is not refused.
    let (st, body) = s
        .post(
            "/v1/auth/signup",
            None,
            json!({"email": "old@x.test", "password": PW, "displayName": "old", "inviteCode": "ABCD-EFGH"}),
        )
        .await;
    assert_eq!(st, 201, "{body}");

    // No code or play key before the email is verified.
    let (st, body) = s.post("/v1/me/netplay", Some(&session), json!({"codeStart": "sara"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (403, Some("email_not_verified")));
    let (st, _) = s.get("/v1/me/user-json", Some(&session)).await;
    assert_eq!(st, 409);

    // Verification email went through the (fake) mailer with a working link.
    let mail = s.mailer.last_to("sarah@x.test").unwrap();
    assert_eq!(mail.subject, "Verify your Brawl Online email");
    assert!(mail.text.contains(&format!("{}/verify-email?token=", s.base)));
    let token = s.mailed_token("sarah@x.test");
    let page = s.http.get(s.url(&format!("/verify-email?token={token}"))).send().await.unwrap();
    assert_eq!(page.status(), 200);
    assert!(page.text().await.unwrap().contains("<title>Email verified - Brawl Online</title>"));
    // Tokens are single-use.
    let (st, _) = s.post("/v1/auth/verify-email", None, json!({"token": token})).await;
    assert_eq!(st, 400);

    // Connect code rules.
    for (prefix, code) in
        [("s", "invalid_code"), ("sarah", "invalid_code"), ("s4r", "invalid_code"), ("", "invalid_code")]
    {
        let (st, body) = s.post("/v1/me/netplay", Some(&session), json!({"codeStart": prefix})).await;
        assert_eq!((st, body["error"]["code"].as_str()), (400, Some(code)), "{prefix}");
    }
    let (st, body) = s.post("/v1/me/netplay", Some(&session), json!({"codeStart": "sara"})).await;
    assert_eq!(st, 200, "{body}");
    let code = body["connectCode"].as_str().unwrap().to_string();
    let (prefix, num) = code.split_once('#').unwrap();
    assert_eq!(prefix, "SARA");
    let n: u16 = num.parse().unwrap();
    assert!((1..=999).contains(&n));
    assert!(code.len() <= 8);
    // Immutable.
    let (st, body) = s.post("/v1/me/netplay", Some(&session), json!({"codeStart": "abc"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (409, Some("code_already_set")));

    // user.json, Slippi-identical.
    let (st, uj) = s.get("/v1/me/user-json", Some(&session)).await;
    assert_eq!(st, 200);
    let keys: Vec<&str> = uj.as_object().unwrap().keys().map(|k| k.as_str()).collect();
    for k in ["uid", "playKey", "connectCode", "displayName", "latestVersion"] {
        assert!(keys.contains(&k), "{k}");
    }
    assert_eq!(keys.len(), 5);
    assert_eq!(uj["connectCode"], code);
    let (_, me) = s.get("/v1/me", Some(&session)).await;
    assert_eq!(me["userJson"], uj);
    assert_eq!(me["playKey"], uj["playKey"]);

    // The play key is stable across logins (the launcher re-fetches it on Play).
    let (st, login) = s.post("/v1/auth/login", None, json!({"email": "SARAH@x.test", "password": PW})).await;
    assert_eq!(st, 200);
    assert_eq!(login["user"]["playKey"], uj["playKey"]);
    assert_eq!(login["user"]["userJson"], uj);

    // Same prefix for another account gives a different number.
    let (_, body) =
        s.post("/v1/auth/signup", None, json!({"email": "b@x.test", "password": PW, "displayName": "sara b"})).await;
    let session_b = body["sessionToken"].as_str().unwrap().to_string();
    let token_b = s.mailed_token("b@x.test");
    s.post("/v1/auth/verify-email", None, json!({"token": token_b})).await;
    let (_, body) = s.post("/v1/me/netplay", Some(&session_b), json!({"codeStart": "SARA"})).await;
    let code_b = body["connectCode"].as_str().unwrap();
    assert!(code_b.starts_with("SARA#"));
    assert_ne!(code_b, code);

    // users-rest compatible public endpoint.
    let uid = uj["uid"].as_str().unwrap();
    let (st, pubu) = s.get(&format!("/user/{uid}?additionalFields=chatMessages,rank"), None).await;
    assert_eq!(st, 200);
    assert_eq!(pubu["connectCode"], code);
    assert_eq!(pubu["chatMessages"].as_array().unwrap().len(), 16);
    assert_eq!(pubu["rank"]["ratingUpdateCount"], 0);
    assert!(pubu.get("playKey").is_none() && pubu.get("email").is_none());
    assert_eq!(s.get("/user/not-a-uuid", None).await.0, 404);

    // Rename and rules.
    let (st, body) = s.post("/v1/me/rename", Some(&session), json!({"displayName": "sarah ohlin"})).await;
    assert_eq!((st, body["displayName"].as_str()), (200, Some("sarah ohlin")));
    let (st, body) = s.post("/v1/me/accept-rules", Some(&session), json!({"num": 1})).await;
    assert_eq!((st, body["rulesVersion"].as_i64()), (200, Some(1)));

    // Logout ends the session.
    let resp = s.http.post(s.url("/v1/auth/logout")).bearer_auth(&session).send().await.unwrap();
    assert_eq!(resp.status(), 204);
    assert_eq!(s.get("/v1/me", Some(&session)).await.0, 401);
    assert_eq!(s.get("/v1/me", None).await.0, 401);
    assert_eq!(s.get("/healthz", None).await.0, 200);
    s.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn password_reset_and_change_rotate_the_play_key() {
    let s = Stack::start(StackOptions::default()).await.unwrap();
    let (session, uj) = s.create_player("p@x.test", "pat", "PAT").await;

    // Wrong password.
    let (st, body) = s.post("/v1/auth/login", None, json!({"email": "p@x.test", "password": "wrong password"})).await;
    assert_eq!((st, body["error"]["code"].as_str()), (401, Some("bad_credentials")));
    let (st, _) = s.post("/v1/auth/login", None, json!({"email": "nobody@x.test", "password": PW})).await;
    assert_eq!(st, 401);

    // Reset request answers 202 for unknown emails too, and mails nothing.
    let before = s.mailer.sent().len();
    let (st, _) = s.post("/v1/auth/password-reset/request", None, json!({"email": "nobody@x.test"})).await;
    assert_eq!(st, 202);
    assert_eq!(s.mailer.sent().len(), before);

    let (st, _) = s.post("/v1/auth/password-reset/request", None, json!({"email": "P@x.test"})).await;
    assert_eq!(st, 202);
    let mail = s.mailer.last_to("p@x.test").unwrap();
    assert_eq!(mail.subject, "Reset your Brawl Online password");
    let token = s.mailed_token("p@x.test");

    // The link opens a form.
    let page = s.http.get(s.url(&format!("/reset-password?token={token}"))).send().await.unwrap();
    assert_eq!(page.status(), 200);
    assert!(page.text().await.unwrap().contains("name=\"password2\""));
    // Mismatched form submission keeps the token usable.
    let resp = s
        .http
        .post(s.url("/reset-password"))
        .form(&[("token", token.as_str()), ("password", "new password 1"), ("password2", "different")])
        .send()
        .await
        .unwrap();
    assert_eq!(resp.status(), 400);
    // Too-short password via the API.
    let (st, _) =
        s.post("/v1/auth/password-reset/confirm", None, json!({"token": token, "newPassword": "short"})).await;
    assert_eq!(st, 400);
    // Real submission through the form.
    let resp = s
        .http
        .post(s.url("/reset-password"))
        .form(&[("token", token.as_str()), ("password", "new password 1"), ("password2", "new password 1")])
        .send()
        .await
        .unwrap();
    assert_eq!(resp.status(), 200);
    assert!(resp.text().await.unwrap().contains("Password changed"));
    // Single use.
    let (st, _) =
        s.post("/v1/auth/password-reset/confirm", None, json!({"token": token, "newPassword": "new password 2"})).await;
    assert_eq!(st, 400);

    // Old session is gone, old password fails, new works, and the play key changed.
    assert_eq!(s.get("/v1/me", Some(&session)).await.0, 401);
    assert_eq!(s.post("/v1/auth/login", None, json!({"email": "p@x.test", "password": PW})).await.0, 401);
    let (st, login) = s.post("/v1/auth/login", None, json!({"email": "p@x.test", "password": "new password 1"})).await;
    assert_eq!(st, 200);
    let key2 = login["user"]["playKey"].as_str().unwrap().to_string();
    assert_ne!(key2, uj.play_key);
    let session2 = login["sessionToken"].as_str().unwrap().to_string();

    // Change password (logged in) rotates again and returns a new session.
    let (st, _) = s
        .post(
            "/v1/auth/change-password",
            Some(&session2),
            json!({"currentPassword": "nope", "newPassword": "new password 3"}),
        )
        .await;
    assert_eq!(st, 401);
    let (st, body) = s
        .post(
            "/v1/auth/change-password",
            Some(&session2),
            json!({"currentPassword": "new password 1", "newPassword": "new password 3"}),
        )
        .await;
    assert_eq!(st, 200, "{body}");
    assert_ne!(body["user"]["playKey"].as_str().unwrap(), key2);
    assert_eq!(s.get("/v1/me", Some(&session2)).await.0, 401);
    let session3 = body["sessionToken"].as_str().unwrap();
    assert_eq!(s.get("/v1/me", Some(session3)).await.0, 200);

    // Admin-issued reset (what `admin user reset-password` does) uses the same confirm path.
    let uid = uuid::Uuid::parse_str(&uj.uid).unwrap();
    let t = accounts::store::create_email_token(
        &s.pool,
        uid,
        accounts::store::TokenPurpose::ResetPassword,
        chrono_hours(24),
    )
    .await
    .unwrap();
    let (st, _) =
        s.post("/v1/auth/password-reset/confirm", None, json!({"token": t, "newPassword": "admin reset pw"})).await;
    assert_eq!(st, 204);
    assert_eq!(s.post("/v1/auth/login", None, json!({"email": "p@x.test", "password": "admin reset pw"})).await.0, 200);

    // Ban: login refused with a clear message.
    accounts::store::set_ban(&s.pool, uid, Some(accounts::store::permanent_ban()), Some("test")).await.unwrap();
    let (st, body) = s.post("/v1/auth/login", None, json!({"email": "p@x.test", "password": "admin reset pw"})).await;
    assert_eq!((st, body["error"]["message"].as_str()), (403, Some("This account is banned")));
    s.shutdown().await;
}

fn chrono_hours(h: i64) -> chrono::Duration {
    chrono::Duration::hours(h)
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn login_and_reset_are_rate_limited() {
    let s = Stack::start(StackOptions { rate_limits: true, ..Default::default() }).await.unwrap();
    let mut statuses = vec![];
    for _ in 0..7 {
        let (st, _) = s.post("/v1/auth/login", None, json!({"email": "x@x.test", "password": "whatever pw"})).await;
        statuses.push(st);
    }
    assert_eq!(statuses, vec![401, 401, 401, 401, 401, 429, 429]);
    let resp =
        s.http.post(s.url("/v1/auth/login")).json(&json!({"email": "x@x.test", "password": "p"})).send().await.unwrap();
    assert!(resp.headers().get("retry-after").is_some());

    // Password reset: one email per address per minute...
    let mut statuses = vec![];
    for _ in 0..3 {
        statuses.push(s.post("/v1/auth/password-reset/request", None, json!({"email": "y@x.test"})).await.0);
    }
    assert_eq!(statuses, vec![202, 429, 429]);
    // ...and 5 requests per minute per IP, whatever the address.
    let mut statuses = vec![];
    for i in 0..3 {
        statuses
            .push(s.post("/v1/auth/password-reset/request", None, json!({"email": format!("z{i}@x.test")})).await.0);
    }
    assert_eq!(statuses, vec![202, 202, 429]);
    s.shutdown().await;
}

/// Open sign-up: accounts per IP (an IPv6 /64 counts as one) and the daily share of
/// `MAIL_DAILY_LIMIT` for verification emails keep sign-ups from using up the email quota,
/// and password resets have their own share.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn signups_and_emails_are_limited() {
    // 9 emails a day: 6 for verification, 3 for password resets.
    let s = Stack::start(StackOptions {
        rate_limits: true,
        trust_proxy_headers: true,
        mail_daily_limit: Some(9),
        ..Default::default()
    })
    .await
    .unwrap();
    let post = |path: &'static str, ip: &'static str, token: Option<String>, body: serde_json::Value| {
        let mut req = s.http.post(s.url(path)).header("x-forwarded-for", format!("10.9.9.9, {ip}")).json(&body);
        if let Some(t) = token {
            req = req.bearer_auth(t);
        }
        async move {
            let resp = req.send().await.unwrap();
            let status = resp.status().as_u16();
            let retry = resp.headers().get("retry-after").is_some();
            let body: serde_json::Value = resp.json().await.unwrap_or_default();
            (status, retry, body)
        }
    };
    let signup = |ip: &'static str, email: &str| {
        post("/v1/auth/signup", ip, None, json!({"email": email, "password": PW, "displayName": "n"}))
    };

    // 3 accounts per IP per hour.
    for i in 0..3 {
        let (st, _, body) = signup("203.0.113.1", &format!("a{i}@x.test")).await;
        assert_eq!(st, 201, "{body}");
    }
    let (st, retry, body) = signup("203.0.113.1", "a3@x.test").await;
    assert_eq!((st, retry), (429, true));
    assert_eq!(body["error"]["message"], "Too many new accounts from this network. Please try again later.");
    // A taken email is refused before the per-IP count, so it costs no slot.
    assert_eq!(signup("203.0.113.2", "a0@x.test").await.0, 409);

    // An IPv6 /64 is one network.
    for (i, ip) in ["2001:db8:1:2::1", "2001:db8:1:2::2", "2001:db8:1:2:ffff::9"].into_iter().enumerate() {
        let (st, _, body) = signup(ip, &format!("b{i}@x.test")).await;
        assert_eq!(st, 201, "{body}");
    }
    assert_eq!(signup("2001:db8:1:2::3", "b3@x.test").await.0, 429);
    assert_eq!(s.mailer.sent().len(), 6);

    // The 6 verification emails of the day are used up: sign-ups from anywhere wait.
    let (st, retry, body) = signup("198.51.100.7", "c0@x.test").await;
    assert_eq!((st, retry), (429, true));
    assert_eq!(body["error"]["message"], "Too many sign-ups right now. Please try again later.");
    let (_, _, login) =
        post("/v1/auth/login", "198.51.100.8", None, json!({"email": "a0@x.test", "password": PW})).await;
    let token = login["sessionToken"].as_str().unwrap().to_string();
    assert_eq!(post("/v1/auth/verify-email/resend", "198.51.100.8", Some(token), json!({})).await.0, 429);
    assert_eq!(s.mailer.sent().len(), 6);

    // Password resets have their own 3: the 4th is still 202 (no account enumeration) but sends nothing.
    for (i, email) in ["a0@x.test", "a1@x.test", "a2@x.test", "b0@x.test"].into_iter().enumerate() {
        let (st, _, _) = post("/v1/auth/password-reset/request", "198.51.100.9", None, json!({"email": email})).await;
        assert_eq!(st, 202);
        assert_eq!(s.mailer.sent().len(), 6 + (i + 1).min(3), "{email}");
    }
    s.shutdown().await;
}
