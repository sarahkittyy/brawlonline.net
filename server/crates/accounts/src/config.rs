//! Configuration, from flags or environment variables (systemd `EnvironmentFile`,
//! or `server/.env` in development).

use std::net::SocketAddr;

use clap::{Args, Parser, ValueEnum};

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum MailerKind {
    /// Send through Resend's HTTP API (needs RESEND_API_KEY).
    Resend,
    /// Send through an SMTP submission server (needs SMTP_HOST; see SMTP_*).
    Smtp,
    /// Print emails to stdout (development).
    Stdout,
    /// Append emails as JSON lines to MAIL_FILE (development, tests, production before a provider).
    File,
}

/// How the SMTP connection is encrypted.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, ValueEnum)]
pub enum SmtpTls {
    /// Plain connect, then STARTTLS, which is required (port 587).
    #[default]
    Starttls,
    /// TLS from the first byte ("implicit TLS", SMTPS, port 465).
    Tls,
    /// No encryption. Only allowed for a test server on a loopback address.
    None,
}

/// A configuration value that never appears in logs or `Debug` output.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct Secret(String);

impl Secret {
    pub fn new(s: impl Into<String>) -> Self {
        Secret(s.into())
    }
    pub fn expose(&self) -> &str {
        &self.0
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

impl std::fmt::Debug for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(if self.0.is_empty() { "Secret(<empty>)" } else { "Secret(<redacted>)" })
    }
}

impl std::str::FromStr for Secret {
    type Err = std::convert::Infallible;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(Secret(s.to_string()))
    }
}

/// Outgoing email settings. Shared by `accounts` and the admin CLI
/// (`user reset-password --send-email`), so both send mail the same way.
#[derive(Debug, Clone, Args)]
pub struct MailConfig {
    /// Mail transport. Defaults to `resend` when RESEND_API_KEY is set, else `stdout`.
    #[arg(long, env = "MAILER", value_enum)]
    pub mailer: Option<MailerKind>,

    #[arg(long, env = "RESEND_API_KEY", hide_env_values = true)]
    pub resend_api_key: Option<Secret>,

    /// Resend API endpoint (overridable for tests against a fake server).
    #[arg(long, env = "RESEND_API_URL", default_value = "https://api.resend.com/emails")]
    pub resend_api_url: String,

    /// Sender: `Name <address>` or a bare address. The provider must accept its domain.
    #[arg(long, env = "MAIL_FROM", default_value = "Brawl Online <noreply@brawlonline.net>")]
    pub mail_from: String,

    /// File for the `file` mailer.
    #[arg(long, env = "MAIL_FILE", default_value = "mail.jsonl")]
    pub mail_file: String,

    /// Stop sending after this many emails per UTC day (per process).
    #[arg(long, env = "MAIL_DAILY_LIMIT", default_value_t = 90)]
    pub mail_daily_limit: u32,

    /// SMTP server for MAILER=smtp, e.g. smtp-relay.brevo.com.
    #[arg(long, env = "SMTP_HOST")]
    pub smtp_host: Option<String>,

    /// SMTP port. Default 587 (465 with SMTP_TLS=tls).
    #[arg(long, env = "SMTP_PORT")]
    pub smtp_port: Option<u16>,

    /// SMTP login. Leave unset for a server without authentication.
    #[arg(long, env = "SMTP_USERNAME", hide_env_values = true)]
    pub smtp_username: Option<String>,

    #[arg(long, env = "SMTP_PASSWORD", hide_env_values = true)]
    pub smtp_password: Option<Secret>,

    /// SMTP encryption: starttls (required, port 587), tls (implicit, port 465),
    /// or none (a local test server only).
    #[arg(long, env = "SMTP_TLS", value_enum, default_value_t = SmtpTls::Starttls)]
    pub smtp_tls: SmtpTls,

    /// Give up on one SMTP send (connect to QUIT) after this many seconds.
    #[arg(long, env = "SMTP_TIMEOUT_SECS", default_value_t = 15)]
    pub smtp_timeout_secs: u64,
}

impl MailConfig {
    pub fn mailer_kind(&self) -> MailerKind {
        self.mailer.unwrap_or(if self.resend_api_key.as_ref().is_some_and(|k| !k.is_empty()) {
            MailerKind::Resend
        } else {
            MailerKind::Stdout
        })
    }

    pub fn smtp_port(&self) -> u16 {
        self.smtp_port.unwrap_or(if self.smtp_tls == SmtpTls::Tls { 465 } else { 587 })
    }

    /// Settings for tests: stdout, and every network endpoint unreachable.
    pub fn for_tests() -> Self {
        MailConfig {
            mailer: Some(MailerKind::Stdout),
            resend_api_key: None,
            resend_api_url: "http://127.0.0.1:9/never".into(),
            mail_from: "Brawl Online <noreply@brawlonline.net>".into(),
            mail_file: "mail.jsonl".into(),
            mail_daily_limit: 1000,
            smtp_host: None,
            smtp_port: None,
            smtp_username: None,
            smtp_password: None,
            smtp_tls: SmtpTls::Starttls,
            smtp_timeout_secs: 15,
        }
    }
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

    #[command(flatten)]
    pub mail: MailConfig,

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
        self.mail.mailer_kind()
    }

    /// A config for tests: everything local, cheap password hashing.
    pub fn for_tests(database_url: &str) -> Self {
        Config {
            database_url: database_url.into(),
            listen: "127.0.0.1:0".parse().unwrap(),
            play_key_secret: "11".repeat(32),
            public_base_url: "http://127.0.0.1:0".into(),
            mail: MailConfig::for_tests(),
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secrets_are_redacted_in_debug() {
        let mut cfg = Config::for_tests("postgres://x");
        cfg.mail.smtp_password = Some(Secret::new("hunter2-smtp"));
        cfg.mail.resend_api_key = Some(Secret::new("re_live_key"));
        let dbg = format!("{cfg:?}");
        assert!(!dbg.contains("hunter2-smtp"));
        assert!(!dbg.contains("re_live_key"));
        assert!(dbg.contains("Secret(<redacted>)"));
    }

    #[test]
    fn smtp_defaults() {
        let cfg = Config::try_parse_from(["accounts", "--database-url", "x", "--play-key-secret", "y"]).unwrap();
        assert_eq!(cfg.mail.smtp_tls, SmtpTls::Starttls);
        assert_eq!(cfg.mail.smtp_port(), 587);
        assert_eq!(cfg.mail.smtp_timeout_secs, 15);
        let cfg = Config::try_parse_from([
            "accounts",
            "--database-url",
            "x",
            "--play-key-secret",
            "y",
            "--mailer",
            "smtp",
            "--smtp-tls",
            "tls",
        ])
        .unwrap();
        assert_eq!(cfg.mailer_kind(), MailerKind::Smtp);
        assert_eq!(cfg.mail.smtp_port(), 465);
    }
}
