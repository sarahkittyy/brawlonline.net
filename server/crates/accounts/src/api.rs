//! JSON API. Endpoint names mirror Slippi's GraphQL operations
//! (docs/backend-design.md section 2.4) so the launcher fork maps one to one.

use std::net::{IpAddr, SocketAddr};
use std::time::Instant;

use axum::extract::{ConnectInfo, Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::Json;
use chrono::Duration;
use common::codes::{validate_code_start, validate_display_name};
use common::net::ip_key;
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::error::{ApiError, ApiResult};
use crate::mail;
use crate::password::{self, validate_password};
use crate::store::{self, CreateUserError, TokenPurpose, UserRow};
use crate::{AppState, Limits};

/// Slippi's `currentRulesVersion` (launcher `src/common/constants.ts:10`).
pub const CURRENT_RULES_VERSION: i32 = 1;
pub const VERIFY_TTL_HOURS: i64 = 48;
pub const RESET_TTL_HOURS: i64 = 1;

/// Exactly the object the launcher writes to `user.json`
/// (Slippi `src/dolphin/playkey.ts`, `setup.ts:69-87`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UserJson {
    pub uid: String,
    pub play_key: String,
    pub connect_code: String,
    pub display_name: String,
    pub latest_version: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Me {
    pub uid: Uuid,
    pub email: String,
    pub email_verified: bool,
    pub email_verification_required: bool,
    pub display_name: String,
    pub connect_code: Option<String>,
    pub play_key: Option<String>,
    pub rules_version: i32,
    pub current_rules_version: i32,
    pub latest_version: String,
    pub role: String,
    /// Present once the account can play online: write it to user.json as is.
    pub user_json: Option<UserJson>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionResponse {
    pub session_token: String,
    pub user: Me,
}

pub fn me(state: &AppState, u: &UserRow) -> Me {
    let verification_ok = u.email_verified() || !state.cfg.require_email_verification;
    let play_key = (verification_ok && u.connect_code.is_some() && !u.is_banned())
        .then(|| state.secret.derive(u.uid, u.play_key_version));
    let user_json = match (&play_key, &u.connect_code) {
        (Some(pk), Some(code)) => Some(UserJson {
            uid: u.uid.to_string(),
            play_key: pk.clone(),
            connect_code: code.clone(),
            display_name: u.display_name.clone(),
            latest_version: state.cfg.latest_version.clone(),
        }),
        _ => None,
    };
    Me {
        uid: u.uid,
        email: u.email.clone(),
        email_verified: u.email_verified(),
        email_verification_required: state.cfg.require_email_verification,
        display_name: u.display_name.clone(),
        connect_code: u.connect_code.clone(),
        play_key,
        rules_version: u.rules_version,
        current_rules_version: CURRENT_RULES_VERSION,
        latest_version: state.cfg.latest_version.clone(),
        role: u.role.clone(),
        user_json,
    }
}

// ---------------------------------------------------------------- helpers

pub fn client_ip(state: &AppState, headers: &HeaderMap, peer: SocketAddr) -> IpAddr {
    if state.cfg.trust_proxy_headers {
        // Caddy appends the real client address as the last X-Forwarded-For entry.
        if let Some(ip) = headers
            .get("x-forwarded-for")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.rsplit(',').next())
            .and_then(|s| s.trim().parse().ok())
        {
            return ip;
        }
    }
    peer.ip()
}

/// Runs one limiter check (`DISABLE_RATE_LIMITS` skips them all). `Err` is the wait.
pub(crate) fn limit(
    state: &AppState,
    check: impl FnOnce(&mut Limits, Instant) -> Result<(), std::time::Duration>,
) -> Result<(), std::time::Duration> {
    if state.cfg.disable_rate_limits {
        return Ok(());
    }
    check(&mut state.limits.lock().unwrap(), Instant::now())
}

/// [`common::ratelimit::AUTH_WINDOWS`] for `key`.
pub fn rate_limit(state: &AppState, key: String) -> ApiResult<()> {
    limit(state, |l, now| l.auth.check(&key, now)).map_err(ApiError::rate_limited)
}

/// One email to one recipient ([`common::ratelimit::MAIL_RECIPIENT_WINDOWS`]).
fn mail_to_limit(state: &AppState, key: String) -> ApiResult<()> {
    limit(state, |l, now| l.mail_to.check(&key, now)).map_err(ApiError::rate_limited)
}

/// One verification email from the daily share for them ([`Limits::new`]).
fn verify_mail_budget(state: &AppState) -> ApiResult<()> {
    limit(state, |l, now| l.verify_mail.check(&(), now)).map_err(|wait| {
        tracing::warn!("daily verification email budget used up; refusing sign-ups and resends");
        ApiError::rate_limited(wait).with_message("Too many sign-ups right now. Please try again later.")
    })
}

fn bearer(headers: &HeaderMap) -> Option<&str> {
    headers.get("authorization")?.to_str().ok()?.strip_prefix("Bearer ").map(str::trim).filter(|t| !t.is_empty())
}

pub async fn current_user(state: &AppState, headers: &HeaderMap) -> ApiResult<UserRow> {
    let token = bearer(headers).ok_or_else(ApiError::unauthorized)?;
    let user =
        store::session_user(&state.pool, token, state.cfg.session_days).await?.ok_or_else(ApiError::unauthorized)?;
    if user.is_banned() {
        return Err(banned_error(&user));
    }
    Ok(user)
}

fn banned_error(u: &UserRow) -> ApiError {
    let until = u.banned_until.unwrap_or_else(store::permanent_ban);
    let msg = if until >= store::permanent_ban() {
        "This account is banned".to_string()
    } else {
        format!("This account is banned until {}", until.format("%Y-%m-%d %H:%M UTC"))
    };
    ApiError::forbidden("banned", msg)
}

fn link(state: &AppState, path: &str, token: &str) -> String {
    format!("{}/{path}?token={token}", state.cfg.public_base_url.trim_end_matches('/'))
}

async fn send_verification(state: &AppState, u: &UserRow) -> ApiResult<()> {
    let token =
        store::create_email_token(&state.pool, u.uid, TokenPurpose::VerifyEmail, Duration::hours(VERIFY_TTL_HOURS))
            .await?;
    let email = mail::verification_email(&u.email, &u.display_name, &link(state, "verify-email", &token));
    if let Err(e) = state.mailer.send(&email).await {
        // The account exists; the user can ask for another email later.
        tracing::error!(uid = %u.uid, "sending verification email failed: {e:#}");
    }
    Ok(())
}

// ---------------------------------------------------------------- auth

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SignupReq {
    pub email: String,
    pub password: String,
    pub display_name: String,
}

/// `createUserNew({email, password, displayName})`. Open to everyone; the limits on
/// accounts per IP and on verification emails protect the email quota.
pub async fn signup(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(req): Json<SignupReq>,
) -> ApiResult<(StatusCode, Json<SessionResponse>)> {
    let ip = ip_key(client_ip(&state, &headers, peer));
    // Every attempt, including invalid ones.
    rate_limit(&state, format!("signup-ip:{ip}"))?;
    let email = store::normalize_email(&req.email).map_err(|m| ApiError::bad_request("invalid_email", m))?;
    validate_display_name(&req.display_name)
        .map_err(|e| ApiError::bad_request("invalid_display_name", e.to_string()))?;
    validate_password(&req.password).map_err(|m| ApiError::bad_request("invalid_password", m))?;
    if store::user_by_email(&state.pool, &email).await?.is_some() {
        return Err(ApiError::conflict("email_taken", CreateUserError::EmailTaken.to_string()));
    }
    // Accounts per IP, then the shared verification email budget (in that order, so one
    // IP cannot use up the shared budget).
    limit(&state, |l, now| l.signup_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait).with_message("Too many new accounts from this network. Please try again later.")
    })?;
    if state.cfg.require_email_verification {
        verify_mail_budget(&state)?;
    }
    let pw_hash = password::hash_async(state.hash_params, req.password).await?;
    let user = store::create_user(&state.pool, &email, &pw_hash, &req.display_name).await.map_err(|e| match e {
        CreateUserError::EmailTaken => ApiError::conflict("email_taken", e.to_string()),
        CreateUserError::Db(e) => ApiError::internal(e),
    })?;
    tracing::info!(uid = %user.uid, "account created");
    if state.cfg.require_email_verification {
        send_verification(&state, &user).await?;
    }
    let ua = headers.get("user-agent").and_then(|v| v.to_str().ok());
    let token = store::create_session(&state.pool, user.uid, state.cfg.session_days, ua).await?;
    Ok((StatusCode::CREATED, Json(SessionResponse { session_token: token, user: me(&state, &user) })))
}

#[derive(Debug, Deserialize)]
pub struct LoginReq {
    pub email: String,
    pub password: String,
}

/// `signInWithEmailAndPassword`.
pub async fn login(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(req): Json<LoginReq>,
) -> ApiResult<Json<SessionResponse>> {
    let ip = client_ip(&state, &headers, peer);
    rate_limit(&state, format!("login-ip:{}", ip_key(ip)))?;
    let email = store::normalize_email(&req.email).unwrap_or_default();
    rate_limit(&state, format!("login-email:{email}"))?;
    let user = store::user_by_email(&state.pool, &email).await?;
    let stored = user.as_ref().map(|u| u.pw_hash.clone()).unwrap_or_else(|| state.dummy_hash.clone());
    let ok = password::verify_async(req.password, stored).await;
    let user = match (ok, user) {
        (true, Some(u)) => u,
        _ => return Err(ApiError::new(StatusCode::UNAUTHORIZED, "bad_credentials", "Wrong email or password")),
    };
    if user.is_banned() {
        return Err(banned_error(&user));
    }
    if !state.cfg.disable_rate_limits {
        state.limits.lock().unwrap().auth.reset(&format!("login-email:{email}"));
    }
    let ua = headers.get("user-agent").and_then(|v| v.to_str().ok());
    let token = store::create_session(&state.pool, user.uid, state.cfg.session_days, ua).await?;
    Ok(Json(SessionResponse { session_token: token, user: me(&state, &user) }))
}

pub async fn logout(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<StatusCode> {
    let token = bearer(&headers).ok_or_else(ApiError::unauthorized)?;
    store::delete_session(&state.pool, token).await?;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn resend_verification(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    if user.email_verified() {
        return Err(ApiError::conflict("already_verified", "Email is already verified"));
    }
    mail_to_limit(&state, format!("verify:{}", user.uid))?;
    verify_mail_budget(&state)?;
    send_verification(&state, &user).await?;
    Ok(StatusCode::ACCEPTED)
}

#[derive(Debug, Deserialize)]
pub struct TokenReq {
    pub token: String,
}

pub async fn verify_email(
    State(state): State<AppState>,
    Json(req): Json<TokenReq>,
) -> ApiResult<Json<serde_json::Value>> {
    match store::verify_email(&state.pool, req.token.trim()).await? {
        Some(uid) => Ok(Json(serde_json::json!({"verified": true, "uid": uid}))),
        None => Err(ApiError::bad_request("bad_token", "This link is invalid or has expired")),
    }
}

#[derive(Debug, Deserialize)]
pub struct ResetRequestReq {
    pub email: String,
}

/// Always answers 202 so it does not reveal which emails have accounts.
pub async fn password_reset_request(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(req): Json<ResetRequestReq>,
) -> ApiResult<StatusCode> {
    rate_limit(&state, format!("reset-ip:{}", ip_key(client_ip(&state, &headers, peer))))?;
    let Ok(email) = store::normalize_email(&req.email) else {
        return Ok(StatusCode::ACCEPTED);
    };
    mail_to_limit(&state, format!("reset:{email}"))?;
    if let Some(user) = store::user_by_email(&state.pool, &email).await? {
        if user.is_banned() {
            // Nothing to send.
        } else if limit(&state, |l, now| l.reset_mail.check(&(), now)).is_err() {
            // Still 202, so the answer does not reveal that the account exists.
            tracing::warn!(uid = %user.uid, "daily password-reset email budget used up; reset email not sent");
        } else {
            let token = store::create_email_token(
                &state.pool,
                user.uid,
                TokenPurpose::ResetPassword,
                Duration::hours(RESET_TTL_HOURS),
            )
            .await?;
            let mail = mail::reset_email(&user.email, &user.display_name, &link(&state, "reset-password", &token));
            if let Err(e) = state.mailer.send(&mail).await {
                tracing::error!(uid = %user.uid, "sending reset email failed: {e:#}");
            }
        }
    }
    Ok(StatusCode::ACCEPTED)
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResetConfirmReq {
    pub token: String,
    pub new_password: String,
}

pub async fn reset_password_confirm(state: &AppState, token: &str, new_password: String) -> ApiResult<Uuid> {
    validate_password(&new_password).map_err(|m| ApiError::bad_request("invalid_password", m))?;
    if store::peek_email_token(&state.pool, token, TokenPurpose::ResetPassword).await?.is_none() {
        return Err(ApiError::bad_request("bad_token", "This link is invalid or has expired"));
    }
    let pw_hash = password::hash_async(state.hash_params, new_password).await?;
    let uid = store::reset_password_with_token(&state.pool, token, &pw_hash)
        .await?
        .ok_or_else(|| ApiError::bad_request("bad_token", "This link is invalid or has expired"))?;
    tracing::info!(%uid, "password reset");
    Ok(uid)
}

pub async fn password_reset_confirm(
    State(state): State<AppState>,
    Json(req): Json<ResetConfirmReq>,
) -> ApiResult<StatusCode> {
    reset_password_confirm(&state, req.token.trim(), req.new_password).await?;
    Ok(StatusCode::NO_CONTENT)
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChangePasswordReq {
    pub current_password: String,
    pub new_password: String,
}

/// Changes the password, which rotates the play key and ends all sessions.
/// Returns a fresh session for the caller.
pub async fn change_password(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<ChangePasswordReq>,
) -> ApiResult<Json<SessionResponse>> {
    let user = current_user(&state, &headers).await?;
    rate_limit(&state, format!("change-pw:{}", user.uid))?;
    if !password::verify_async(req.current_password, user.pw_hash.clone()).await {
        return Err(ApiError::new(StatusCode::UNAUTHORIZED, "bad_credentials", "Wrong password"));
    }
    validate_password(&req.new_password).map_err(|m| ApiError::bad_request("invalid_password", m))?;
    let pw_hash = password::hash_async(state.hash_params, req.new_password).await?;
    store::set_password(&state.pool, user.uid, &pw_hash).await?;
    let ua = headers.get("user-agent").and_then(|v| v.to_str().ok());
    let token = store::create_session(&state.pool, user.uid, state.cfg.session_days, ua).await?;
    let user = store::user_by_uid(&state.pool, user.uid).await?.ok_or_else(ApiError::not_found)?;
    Ok(Json(SessionResponse { session_token: token, user: me(&state, &user) }))
}

// ---------------------------------------------------------------- me

/// `getUser`, including the play key once the account can play.
pub async fn get_me(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<Me>> {
    let user = current_user(&state, &headers).await?;
    Ok(Json(me(&state, &user)))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NetplayReq {
    pub code_start: String,
}

/// `userInitNetplay(codeStart)`: assigns the connect code.
pub async fn init_netplay(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<NetplayReq>,
) -> ApiResult<Json<Me>> {
    let user = current_user(&state, &headers).await?;
    if state.cfg.require_email_verification && !user.email_verified() {
        return Err(ApiError::forbidden("email_not_verified", "Verify your email before activating online play"));
    }
    if user.connect_code.is_some() {
        return Err(ApiError::conflict("code_already_set", "This account already has a connect code"));
    }
    let prefix =
        validate_code_start(&req.code_start).map_err(|e| ApiError::bad_request("invalid_code", e.to_string()))?;
    let code = store::assign_connect_code(&state.pool, user.uid, &prefix).await.map_err(|e| match e {
        store::AssignCodeError::AlreadyAssigned => ApiError::conflict("code_already_set", e.to_string()),
        store::AssignCodeError::Exhausted => ApiError::conflict("prefix_exhausted", e.to_string()),
        store::AssignCodeError::Db(e) => ApiError::internal(e),
    })?;
    tracing::info!(uid = %user.uid, %code, "connect code assigned");
    let user = store::user_by_uid(&state.pool, user.uid).await?.ok_or_else(ApiError::not_found)?;
    Ok(Json(me(&state, &user)))
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RenameReq {
    pub display_name: String,
}

/// `userRename`.
pub async fn rename(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<RenameReq>,
) -> ApiResult<Json<Me>> {
    let user = current_user(&state, &headers).await?;
    validate_display_name(&req.display_name)
        .map_err(|e| ApiError::bad_request("invalid_display_name", e.to_string()))?;
    store::rename(&state.pool, user.uid, &req.display_name).await?;
    let user = store::user_by_uid(&state.pool, user.uid).await?.ok_or_else(ApiError::not_found)?;
    Ok(Json(me(&state, &user)))
}

#[derive(Debug, Deserialize)]
pub struct AcceptRulesReq {
    pub num: i32,
}

/// `userAcceptRules(num)`.
pub async fn accept_rules(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<AcceptRulesReq>,
) -> ApiResult<Json<Me>> {
    let user = current_user(&state, &headers).await?;
    if req.num < 1 || req.num > CURRENT_RULES_VERSION {
        return Err(ApiError::bad_request("invalid_rules_version", "Unknown rules version"));
    }
    store::accept_rules(&state.pool, user.uid, req.num).await?;
    let user = store::user_by_uid(&state.pool, user.uid).await?.ok_or_else(ApiError::not_found)?;
    Ok(Json(me(&state, &user)))
}

/// The exact `user.json` contents, or 409 while the account cannot play yet.
pub async fn user_json(State(state): State<AppState>, headers: HeaderMap) -> ApiResult<Json<UserJson>> {
    let user = current_user(&state, &headers).await?;
    me(&state, &user)
        .user_json
        .map(Json)
        .ok_or_else(|| ApiError::conflict("not_activated", "Verify your email and pick a connect code first"))
}

// ---------------------------------------------------------------- public

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PublicRank {
    pub rating_ordinal: f32,
    pub rating_update_count: u32,
    pub daily_global_placement: Option<u16>,
    pub daily_regional_placement: Option<u16>,
    /// 1-based position on the leaderboard (`GET /v1/ranked/leaderboard`), null before the
    /// first rated set.
    pub position: Option<u64>,
    /// Players on the leaderboard (at least one rated set).
    pub ranked_players: u64,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PublicUser {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
    pub latest_version: String,
    pub chat_messages: Vec<String>,
    pub rank: PublicRank,
}

/// Slippi's users-rest `GET /user/{uid}?additionalFields=chatMessages,rank`,
/// which Dolphin's `user` crate polls (`slippi-rust-extensions/user/src/lib.rs:354-421`).
/// Public, as on Slippi. `rank.ratingOrdinal` is the Elo rating (`common::ranked`; the default
/// before the first set) and `ratingUpdateCount` the rated sets. There are no daily placements;
/// `position` and `rankedPlayers` place the player on the leaderboard.
pub async fn public_user(State(state): State<AppState>, Path(uid): Path<String>) -> ApiResult<Json<PublicUser>> {
    let uid = Uuid::parse_str(&uid).map_err(|_| ApiError::not_found())?;
    let user = store::user_by_uid(&state.pool, uid).await?.ok_or_else(ApiError::not_found)?;
    let standing = crate::ranked::standing(&state.pool, uid).await?;
    let (position, ranked_players) = crate::leaderboard::placement(&state.pool, uid, standing).await?;
    Ok(Json(PublicUser {
        uid: user.uid,
        display_name: user.display_name,
        connect_code: user.connect_code.unwrap_or_default(),
        latest_version: state.cfg.latest_version.clone(),
        chat_messages: common::DEFAULT_CHAT_MESSAGES.iter().map(|s| s.to_string()).collect(),
        rank: PublicRank {
            rating_ordinal: standing.rating as f32,
            rating_update_count: standing.sets_played,
            daily_global_placement: None,
            daily_regional_placement: None,
            position,
            ranked_players,
        },
    }))
}

pub async fn healthz(State(state): State<AppState>) -> ApiResult<&'static str> {
    sqlx::query("SELECT 1").execute(&state.pool).await?;
    Ok("ok")
}
