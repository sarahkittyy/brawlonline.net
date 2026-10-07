//! Configuration, from flags or environment variables (systemd `EnvironmentFile`,
//! or `server/.env` in development).

use std::net::SocketAddr;

use clap::{Parser, ValueEnum};

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum MailerKind {
    /// Send through Resend's HTTP API (needs RESEND_API_KEY).
    Resend,
    /// Print emails to stdout (development).
    Stdout,
    /// Append emails as JSON lines to MAIL_FILE (development, tests).
    File,
}

#[derive(Debug, Clone, Parser)]
#[command(name = "accounts", about = "Accounts web service", version)]
pub struct Config {
    /// Postgres connection string.
    #[arg(long, env = "DATABASE_URL")]
    pub database_url: String,

    /// Address to listen on. Production listens on loopback behind Caddy.
    #[arg(long, env = "ACCOUNTS_LISTEN", default_value = "127.0.0.1:8080")]
    pub listen: SocketAddr,

    /// Secret used to derive play keys (hex or base64url, >= 32 bytes). Must be
    /// the same value the mm server uses. `admin gen-secret` prints one.
    #[arg(long, env = "PLAY_KEY_SECRET", hide_env_values = true)]
    pub play_key_secret: String,

    /// Public base URL of this service, used in email links.
    #[arg(long, env = "PUBLIC_BASE_URL", default_value = "http://127.0.0.1:8080")]
    pub public_base_url: String,

    /// Mail transport. Defaults to `resend` when RESEND_API_KEY is set, else `stdout`.
    #[arg(long, env = "MAILER", value_enum)]
    pub mailer: Option<MailerKind>,

    #[arg(long, env = "RESEND_API_KEY", hide_env_values = true)]
    pub resend_api_key: Option<String>,

    /// Resend API endpoint (overridable for tests against a fake server).
    #[arg(long, env = "RESEND_API_URL", default_value = "https://api.resend.com/emails")]
    pub resend_api_url: String,

    /// Sender, as Resend takes it: `Name <address>` or a bare address.
    #[arg(long, env = "MAIL_FROM", default_value = "Brawl Online <noreply@fluffycat.gay>")]
    pub mail_from: String,

    /// File for the `file` mailer.
    #[arg(long, env = "MAIL_FILE", default_value = "mail.jsonl")]
    pub mail_file: String,

    /// Stop sending after this many emails per UTC day (Resend's free tier is 100/day).
    #[arg(long, env = "MAIL_DAILY_LIMIT", default_value_t = 90)]
    pub mail_daily_limit: u32,

    /// Require an invite code to sign up (friends-only phase).
    #[arg(long, env = "SIGNUP_INVITE_ONLY", default_value_t = true, action = clap::ArgAction::Set)]
    pub signup_invite_only: bool,

    /// Require a verified email before a connect code and play key are issued.
    #[arg(long, env = "REQUIRE_EMAIL_VERIFICATION", default_value_t = true, action = clap::ArgAction::Set)]
    pub require_email_verification: bool,

    /// Client version written to user.json `latestVersion`.
    #[arg(long, env = "LATEST_VERSION", default_value = "0.1.0")]
    pub latest_version: String,

    /// Take the client IP from X-Forwarded-For (only behind Caddy).
    #[arg(long, env = "TRUST_PROXY_HEADERS", default_value_t = false, action = clap::ArgAction::Set)]
    pub trust_proxy_headers: bool,

    /// Argon2id memory cost in KiB (design: 64 MiB).
    #[arg(long, env = "ARGON2_MEMORY_KIB", default_value_t = 65536)]
    pub argon2_memory_kib: u32,

    /// Argon2id iterations (design: 3).
    #[arg(long, env = "ARGON2_ITERATIONS", default_value_t = 3)]
    pub argon2_iterations: u32,

    /// Sliding session lifetime in days.
    #[arg(long, env = "SESSION_DAYS", default_value_t = 90)]
    pub session_days: i64,

    /// Run database migrations on startup.
    #[arg(long, env = "RUN_MIGRATIONS", default_value_t = true, action = clap::ArgAction::Set)]
    pub run_migrations: bool,

    /// Disable rate limiting (tests only).
    #[arg(long, env = "DISABLE_RATE_LIMITS", default_value_t = false, action = clap::ArgAction::Set, hide = true)]
    pub disable_rate_limits: bool,
}

impl Config {
    pub fn mailer_kind(&self) -> MailerKind {
        self.mailer.unwrap_or(if self.resend_api_key.as_deref().is_some_and(|k| !k.is_empty()) {
            MailerKind::Resend
        } else {
            MailerKind::Stdout
        })
    }

    /// A config for tests: everything local, cheap password hashing.
    pub fn for_tests(database_url: &str) -> Self {
        Config {
            database_url: database_url.into(),
            listen: "127.0.0.1:0".parse().unwrap(),
            play_key_secret: "11".repeat(32),
            public_base_url: "http://127.0.0.1:0".into(),
            mailer: Some(MailerKind::Stdout),
            resend_api_key: None,
            resend_api_url: "http://127.0.0.1:9/never".into(),
            mail_from: "Brawl Online <noreply@fluffycat.gay>".into(),
            mail_file: "mail.jsonl".into(),
            mail_daily_limit: 1000,
            signup_invite_only: true,
            require_email_verification: true,
            latest_version: "0.1.0".into(),
            trust_proxy_headers: false,
            argon2_memory_kib: 8192,
            argon2_iterations: 1,
            session_days: 90,
            run_migrations: true,
            disable_rate_limits: false,
        }
    }
}
