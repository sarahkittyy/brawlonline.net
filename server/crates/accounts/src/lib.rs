//! The accounts web service: sign-up (open to everyone, with email
//! verification), login, sessions, connect codes, play keys and the data the launcher writes
//! to `user.json`. See docs/backend-design.md sections 2.4 and 3.

pub mod api;
pub mod config;
pub mod cursor;
pub mod error;
pub mod history;
pub mod leaderboard;
pub mod mail;
pub mod pages;
pub mod password;
pub mod ranked;
pub mod rooms;
pub mod store;

use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, Mutex};

use axum::routing::{get, post};
use axum::Router;
use common::playkey::PlayKeySecret;
use common::ratelimit::{
    RateLimiter, Window, AUTH_WINDOWS, DAY, HISTORY_WINDOWS, LEADERBOARD_IP_WINDOWS, MAIL_RECIPIENT_WINDOWS,
    ROOMS_WINDOWS, SIGNUP_IP_WINDOWS,
};
use sqlx::postgres::PgPoolOptions;
use sqlx::PgPool;
use tower_http::limit::RequestBodyLimitLayer;
use tower_http::trace::TraceLayer;

pub use config::Config;

#[derive(Clone)]
pub struct AppState {
    pub pool: PgPool,
    pub cfg: Arc<Config>,
    pub secret: PlayKeySecret,
    pub mailer: Arc<dyn mail::Mailer>,
    pub limits: Arc<Mutex<Limits>>,
    pub hash_params: password::HashParams,
    pub dummy_hash: String,
    /// mm's status (online count, public rooms) for `GET /v1/rooms`.
    pub mm_status: Arc<rooms::MmStatusSource>,
}

impl AppState {
    pub fn new(pool: PgPool, cfg: Config, mailer: Arc<dyn mail::Mailer>) -> anyhow::Result<Self> {
        let secret = PlayKeySecret::parse(&cfg.play_key_secret)?;
        let hash_params = password::HashParams { memory_kib: cfg.argon2_memory_kib, iterations: cfg.argon2_iterations };
        Ok(AppState {
            pool,
            secret,
            mailer,
            limits: Arc::new(Mutex::new(Limits::new(cfg.mail.mail_daily_limit))),
            hash_params,
            dummy_hash: password::dummy_hash(hash_params),
            mm_status: Arc::new(rooms::MmStatusSource::new(&cfg.mm_status_url)),
            cfg: Arc::new(cfg),
        })
    }
}

/// The rate limiters (in memory, per process). Values and reasons: `server/README.md`, "Rate limits".
pub struct Limits {
    /// [`AUTH_WINDOWS`], keyed `<what>:<ip, email or uid>`.
    pub auth: RateLimiter<String>,
    /// Accounts created per client IP (IPv6 per /64): [`SIGNUP_IP_WINDOWS`].
    pub signup_ip: RateLimiter<IpAddr>,
    /// Emails to one recipient: [`MAIL_RECIPIENT_WINDOWS`], keyed `verify:<uid>` or `reset:<email>`.
    pub mail_to: RateLimiter<String>,
    /// Verification emails (sign-ups and resends) from all clients together.
    pub verify_mail: RateLimiter<()>,
    /// Password-reset emails from all clients together.
    pub reset_mail: RateLimiter<()>,
    /// Leaderboard pages per client IP (IPv6 per /64): [`LEADERBOARD_IP_WINDOWS`].
    pub leaderboard_ip: RateLimiter<IpAddr>,
    /// Match history pages per account: [`HISTORY_WINDOWS`].
    pub history: RateLimiter<uuid::Uuid>,
    /// Room list requests per account: [`ROOMS_WINDOWS`].
    pub rooms: RateLimiter<uuid::Uuid>,
}

impl Limits {
    /// Splits the daily email cap (`MAIL_DAILY_LIMIT`) between the two kinds of email, so a
    /// flood of one cannot use up the other's share: two thirds for verification emails, the
    /// rest for password resets (60 and 30 with the default 90). Sign-up answers 429 once the
    /// verification share of the last 24 hours is used up, rather than creating accounts whose
    /// email cannot be sent.
    pub fn new(mail_daily_limit: u32) -> Self {
        let (verify, reset) = Self::mail_budgets(mail_daily_limit);
        Limits {
            auth: RateLimiter::new(&AUTH_WINDOWS),
            signup_ip: RateLimiter::new(&SIGNUP_IP_WINDOWS),
            mail_to: RateLimiter::new(&MAIL_RECIPIENT_WINDOWS),
            verify_mail: RateLimiter::new(&[Window::new(verify, DAY)]),
            reset_mail: RateLimiter::new(&[Window::new(reset, DAY)]),
            leaderboard_ip: RateLimiter::new(&LEADERBOARD_IP_WINDOWS),
            history: RateLimiter::new(&HISTORY_WINDOWS),
            rooms: RateLimiter::new(&ROOMS_WINDOWS),
        }
    }

    /// (verification, reset) emails per 24 hours for a daily cap. Each at least 1.
    pub fn mail_budgets(mail_daily_limit: u32) -> (usize, usize) {
        let total = mail_daily_limit as usize;
        let verify = (total * 2 / 3).max(1);
        (verify, total.saturating_sub(verify).max(1))
    }
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/v1/auth/signup", post(api::signup))
        .route("/v1/auth/login", post(api::login))
        .route("/v1/auth/logout", post(api::logout))
        .route("/v1/auth/verify-email", post(api::verify_email))
        .route("/v1/auth/verify-email/resend", post(api::resend_verification))
        .route("/v1/auth/password-reset/request", post(api::password_reset_request))
        .route("/v1/auth/password-reset/confirm", post(api::password_reset_confirm))
        .route("/v1/auth/change-password", post(api::change_password))
        .route("/v1/me", get(api::get_me))
        .route("/v1/me/netplay", post(api::init_netplay))
        .route("/v1/me/rename", post(api::rename))
        .route("/v1/me/accept-rules", post(api::accept_rules))
        .route("/v1/me/user-json", get(api::user_json))
        .route("/v1/me/matches", get(history::my_matches))
        .route("/user/{uid}", get(api::public_user))
        .route("/v1/ranked/report-game", post(ranked::report_game))
        .route("/v1/ranked/report-leave", post(ranked::report_leave))
        .route("/v1/ranked/result", get(ranked::result))
        .route("/v1/ranked/leaderboard", get(leaderboard::leaderboard))
        .route("/v1/rooms", get(rooms::rooms))
        .route("/verify-email", get(pages::verify_email_page))
        .route("/reset-password", get(pages::reset_password_page).post(pages::reset_password_submit))
        .route("/healthz", get(api::healthz))
        .layer(RequestBodyLimitLayer::new(16 * 1024))
        .layer(TraceLayer::new_for_http())
        .with_state(state)
}

pub async fn connect_db(url: &str) -> anyhow::Result<PgPool> {
    Ok(PgPoolOptions::new().max_connections(10).connect(url).await?)
}

/// Serves on an already-bound listener, with the ranked sweeper ([`ranked::sweeper`]) alongside.
pub async fn serve(listener: tokio::net::TcpListener, state: AppState) -> anyhow::Result<()> {
    let sweeper = ranked::sweeper(
        state.pool.clone(),
        state.cfg.ranked_timing(),
        std::time::Duration::from_secs(state.cfg.ranked_sweep_secs),
    );
    let app = router(state).into_make_service_with_connect_info::<SocketAddr>();
    tokio::select! {
        r = axum::serve(listener, app) => r?,
        () = sweeper => {}
    }
    Ok(())
}

/// Everything `main` does: connect, migrate, build the mailer, serve.
pub async fn run(cfg: Config) -> anyhow::Result<()> {
    let pool = connect_db(&cfg.database_url).await?;
    if cfg.run_migrations {
        common::db::MIGRATOR.run(&pool).await?;
    }
    let mailer = mail::from_config(&cfg.mail)?;
    tracing::info!(
        mailer = ?cfg.mailer_kind(),
        mail = %mail::describe(&cfg.mail),
        mail_daily_limit = cfg.mail.mail_daily_limit,
        "accounts starting"
    );
    let listener = tokio::net::TcpListener::bind(cfg.listen).await?;
    tracing::info!("listening on http://{}", listener.local_addr()?);
    let state = AppState::new(pool, cfg, mailer)?;
    serve(listener, state).await
}

#[cfg(test)]
mod tests {
    use super::Limits;

    #[test]
    fn mail_budgets_split_the_daily_cap() {
        assert_eq!(Limits::mail_budgets(90), (60, 30));
        assert_eq!(Limits::mail_budgets(250), (166, 84));
        // Never a zero-sized window.
        assert_eq!(Limits::mail_budgets(1), (1, 1));
        assert_eq!(Limits::mail_budgets(0), (1, 1));
    }
}
