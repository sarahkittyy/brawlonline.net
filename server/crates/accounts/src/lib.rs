//! The accounts web service: sign-up (invite code and email verification),
//! login, sessions, connect codes, play keys and the data the launcher writes
//! to `user.json`. See docs/backend-design.md sections 2.4 and 3.

pub mod api;
pub mod config;
pub mod error;
pub mod mail;
pub mod pages;
pub mod password;
pub mod store;

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};

use axum::routing::{get, post};
use axum::Router;
use common::playkey::PlayKeySecret;
use common::ratelimit::{RateLimiter, AUTH_WINDOWS};
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
    pub limiter: Arc<Mutex<RateLimiter<String>>>,
    pub hash_params: password::HashParams,
    pub dummy_hash: String,
}

impl AppState {
    pub fn new(pool: PgPool, cfg: Config, mailer: Arc<dyn mail::Mailer>) -> anyhow::Result<Self> {
        let secret = PlayKeySecret::parse(&cfg.play_key_secret)?;
        let hash_params = password::HashParams { memory_kib: cfg.argon2_memory_kib, iterations: cfg.argon2_iterations };
        Ok(AppState {
            pool,
            secret,
            mailer,
            limiter: Arc::new(Mutex::new(RateLimiter::new(&AUTH_WINDOWS))),
            hash_params,
            dummy_hash: password::dummy_hash(hash_params),
            cfg: Arc::new(cfg),
        })
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
        .route("/user/{uid}", get(api::public_user))
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

/// Serves on an already-bound listener.
pub async fn serve(listener: tokio::net::TcpListener, state: AppState) -> anyhow::Result<()> {
    let app = router(state).into_make_service_with_connect_info::<SocketAddr>();
    axum::serve(listener, app).await?;
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
        invite_only = cfg.signup_invite_only,
        "accounts starting"
    );
    let listener = tokio::net::TcpListener::bind(cfg.listen).await?;
    tracing::info!("listening on http://{}", listener.local_addr()?);
    let state = AppState::new(pool, cfg, mailer)?;
    serve(listener, state).await
}
