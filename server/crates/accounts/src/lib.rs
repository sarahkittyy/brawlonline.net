//! The accounts web service: sign-up (open to everyone, with email
//! verification), login, sessions, connect codes, play keys and the data the launcher writes
//! to `user.json`. See docs/backend-design.md sections 2.4 and 3.

pub mod api;
pub mod bug_reports;
pub mod config;
pub mod cursor;
pub mod error;
pub mod history;
pub mod leaderboard;
pub mod live;
pub mod mail;
pub mod pages;
pub mod password;
pub mod ranked;
pub mod rooms;
pub mod rulesets;
pub mod sd_cards;
pub mod slur_filter;
pub mod social;
pub mod store;

use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, Mutex};

use axum::routing::{get, post, put};
use axum::Router;
use common::playkey::PlayKeySecret;
use common::ratelimit::{
    RateLimiter, Window, AUTH_WINDOWS, BUG_REPORT_IP_WINDOWS, DAY, HISTORY_WINDOWS, LEADERBOARD_IP_WINDOWS,
    MAIL_RECIPIENT_WINDOWS, ROOMS_WINDOWS, ROOM_NAME_WINDOWS, RULESET_EDIT_WINDOWS, RULESET_LOOKUP_IP_WINDOWS,
    RULESET_PUBLISH_WINDOWS, SD_LISTING_FETCH_WINDOWS, SD_LISTING_UPLOAD_WINDOWS, SIGNUP_IP_WINDOWS,
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
    /// The launchers online: presence, DMs and invites (`GET /v1/social/stream`).
    pub social: Arc<social::SocialHub>,
    /// Spectators and spectator chat (`/v1/live`).
    pub live: Arc<live::LiveHub>,
    /// The racial slur check of names and codes (off without `DEEPINFRA_API_KEY`).
    pub slur_filter: Arc<slur_filter::SlurFilter>,
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
            social: Arc::new(social::SocialHub::new(!cfg.disable_rate_limits)),
            live: Arc::new(live::LiveHub::new(!cfg.disable_rate_limits)),
            slur_filter: Arc::new(slur_filter::SlurFilter::new(&cfg.slur_filter)?),
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
    /// Room names set per account: [`ROOM_NAME_WINDOWS`].
    pub room_name: RateLimiter<uuid::Uuid>,
    /// Live game list requests per account: [`ROOMS_WINDOWS`] as well.
    pub live: RateLimiter<uuid::Uuid>,
    /// Rulesets published per account: [`RULESET_PUBLISH_WINDOWS`].
    pub ruleset_publish: RateLimiter<uuid::Uuid>,
    /// Ruleset lookups per client IP (IPv6 per /64): [`RULESET_LOOKUP_IP_WINDOWS`].
    pub ruleset_lookup_ip: RateLimiter<IpAddr>,
    /// The account's own ruleset calls: [`RULESET_EDIT_WINDOWS`].
    pub ruleset_edit: RateLimiter<uuid::Uuid>,
    /// Bug reports per client IP (IPv6 per /64): [`BUG_REPORT_IP_WINDOWS`].
    pub bug_report_ip: RateLimiter<IpAddr>,
    /// SD card listings uploaded per account: [`SD_LISTING_UPLOAD_WINDOWS`].
    pub sd_listing_upload: RateLimiter<uuid::Uuid>,
    /// SD card listings fetched per account: [`SD_LISTING_FETCH_WINDOWS`].
    pub sd_listing_fetch: RateLimiter<uuid::Uuid>,
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
            room_name: RateLimiter::new(&ROOM_NAME_WINDOWS),
            live: RateLimiter::new(&ROOMS_WINDOWS),
            ruleset_publish: RateLimiter::new(&RULESET_PUBLISH_WINDOWS),
            ruleset_lookup_ip: RateLimiter::new(&RULESET_LOOKUP_IP_WINDOWS),
            ruleset_edit: RateLimiter::new(&RULESET_EDIT_WINDOWS),
            bug_report_ip: RateLimiter::new(&BUG_REPORT_IP_WINDOWS),
            sd_listing_upload: RateLimiter::new(&SD_LISTING_UPLOAD_WINDOWS),
            sd_listing_fetch: RateLimiter::new(&SD_LISTING_FETCH_WINDOWS),
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
    // A bug report's logs zip is the one large body; everything else stays at 16 KiB. The upload
    // checks BUG_REPORT_MAX_LOGS_BYTES itself (with a message the launcher shows); this is a
    // backstop above it.
    let logs_limit = usize::try_from(state.cfg.bug_report_max_logs_bytes.saturating_mul(2)).unwrap_or(usize::MAX);
    let uploads = Router::new()
        .route("/v1/bug-reports/{id}/logs", put(bug_reports::upload_logs))
        .layer(RequestBodyLimitLayer::new(logs_limit));
    // An SD card listing (a few MB for an 8 GB card): its own limit, checked in the upload too.
    let listing_limit = usize::try_from(state.cfg.sd_listing_max_bytes).unwrap_or(usize::MAX);
    let listings = Router::new()
        .route("/v1/sd-cards/{root}/listing", get(sd_cards::download).put(sd_cards::upload))
        .layer(RequestBodyLimitLayer::new(listing_limit));
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
        .route("/v1/mod-rooms/{code}", get(rooms::mod_room))
        .route("/v1/me/room-name", put(rooms::set_room_name))
        .route("/v1/rulesets", get(rulesets::browse).post(rulesets::publish))
        .route("/v1/rulesets/{code}", get(rulesets::lookup))
        .route("/v1/rulesets/{code}/preview", get(rulesets::preview))
        .route("/v1/me/rulesets", get(rulesets::mine))
        .route("/v1/me/rulesets/recent", get(rulesets::recent))
        .route("/v1/me/rulesets/{code}/hidden", put(rulesets::set_hidden))
        .route("/v1/me/ruleset-drafts", post(rulesets::create_draft))
        .route(
            "/v1/me/ruleset-drafts/{id}",
            get(rulesets::get_draft).put(rulesets::update_draft).delete(rulesets::delete_draft),
        )
        .route("/v1/me/ruleset", get(rulesets::remembered).put(rulesets::set_remembered))
        .route("/v1/social/stream", get(social::stream))
        .route("/v1/social/dm", post(social::dm))
        .route("/v1/social/invite", post(social::invite))
        .route("/v1/social/invite/answer", post(social::answer))
        .route("/v1/live", get(live::list))
        .route("/v1/live/{id}/watch", get(live::watch))
        .route("/v1/live/{id}/chat", post(live::chat))
        .route("/v1/live/{id}/report", post(live::report))
        .route("/verify-email", get(pages::verify_email_page))
        .route("/reset-password", get(pages::reset_password_page).post(pages::reset_password_submit))
        .route("/healthz", get(api::healthz))
        .route("/v1/bug-reports", post(bug_reports::create))
        .layer(RequestBodyLimitLayer::new(16 * 1024))
        .merge(uploads)
        .merge(listings)
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
    let room_watcher = social::room_watcher(state.clone());
    let account_watcher = social::account_watcher(state.clone());
    let live_ticker = live::ticker(state.live.clone());
    let app = router(state).into_make_service_with_connect_info::<SocketAddr>();
    tokio::select! {
        r = axum::serve(listener, app) => r?,
        () = sweeper => {}
        () = room_watcher => {}
        () = account_watcher => {}
        () = live_ticker => {}
    }
    Ok(())
}

/// Everything `main` does: connect, migrate, build the mailer, serve.
pub async fn run(cfg: Config) -> anyhow::Result<()> {
    let pool = connect_db(&cfg.database_url).await?;
    if cfg.run_migrations {
        common::db::MIGRATOR.run(&pool).await?;
        ranked::backfill_tiers(&pool).await?;
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
    tracing::info!("slur filter {}", state.slur_filter.describe());
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
