//! Outgoing email: verification links and password resets.
//!
//! Production sends through an SMTP submission server ([`SmtpMailer`], any
//! provider) or Resend's HTTP API ([`ResendMailer`]). Development prints to
//! stdout or appends JSON lines to a file, and tests use [`MemoryMailer`].
//! Nothing in the test suite talks to a real provider: the SMTP and Resend
//! clients are tested against local fakes.
//!
//! [`from_config`] is the one place that turns [`MailConfig`] into a mailer;
//! `accounts` and the admin CLI both use it.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Context;
use async_trait::async_trait;
use chrono::{NaiveDate, Utc};
use common::PRODUCT_NAME;
use lettre::message::{Mailbox, MultiPart};
use lettre::transport::smtp::authentication::Credentials;
use lettre::{AsyncSmtpTransport, AsyncTransport, Message, Tokio1Executor};
use serde::{Deserialize, Serialize};

use crate::config::{MailConfig, MailerKind, SmtpTls};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Email {
    pub to: String,
    pub subject: String,
    pub text: String,
    pub html: String,
}

#[async_trait]
pub trait Mailer: Send + Sync {
    async fn send(&self, email: &Email) -> anyhow::Result<()>;
}

/// Resend (<https://resend.com/docs/api-reference/emails/send-email>).
pub struct ResendMailer {
    client: reqwest::Client,
    api_url: String,
    api_key: String,
    from: String,
}

impl ResendMailer {
    pub fn new(api_url: &str, api_key: &str, from: &str) -> Self {
        ResendMailer {
            client: reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(15))
                .build()
                .expect("reqwest client"),
            api_url: api_url.into(),
            api_key: api_key.into(),
            from: from.into(),
        }
    }
}

#[derive(Serialize)]
struct ResendRequest<'a> {
    from: &'a str,
    to: [&'a str; 1],
    subject: &'a str,
    text: &'a str,
    html: &'a str,
}

#[async_trait]
impl Mailer for ResendMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        let resp = self
            .client
            .post(&self.api_url)
            .bearer_auth(&self.api_key)
            .json(&ResendRequest {
                from: &self.from,
                to: [&email.to],
                subject: &email.subject,
                text: &email.text,
                html: &email.html,
            })
            .send()
            .await?;
        let status = resp.status();
        if !status.is_success() {
            let body = resp.text().await.unwrap_or_default();
            anyhow::bail!("resend returned {status}: {}", body.chars().take(300).collect::<String>());
        }
        Ok(())
    }
}

/// Brevo's transactional email API
/// (<https://developers.brevo.com/reference/sendtransacemail>): `POST /v3/smtp/email`
/// with an `api-key` header (an API v3 key, `xkeysib-...`). Brevo restricts keys
/// to authorised IPs, so it only works from the production box.
pub struct BrevoMailer {
    client: reqwest::Client,
    api_url: String,
    api_key: String,
    sender: BrevoAddress,
}

#[derive(Serialize, Clone)]
struct BrevoAddress {
    #[serde(skip_serializing_if = "Option::is_none")]
    name: Option<String>,
    email: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct BrevoRequest<'a> {
    sender: &'a BrevoAddress,
    to: [BrevoAddress; 1],
    subject: &'a str,
    html_content: &'a str,
    text_content: &'a str,
}

impl BrevoMailer {
    /// `from` is `MAIL_FROM` (`Name <address>` or `address`).
    pub fn new(api_url: &str, api_key: &str, from: &str) -> anyhow::Result<Self> {
        let mb: Mailbox = from.parse().map_err(|e| {
            anyhow::anyhow!("MAIL_FROM {from:?} is not a valid sender (`Name <address>` or `address`): {e}")
        })?;
        Ok(BrevoMailer {
            client: reqwest::Client::builder().timeout(Duration::from_secs(15)).build().context("http client")?,
            api_url: api_url.into(),
            api_key: api_key.into(),
            sender: BrevoAddress { name: mb.name.filter(|n| !n.is_empty()), email: mb.email.to_string() },
        })
    }
}

#[async_trait]
impl Mailer for BrevoMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        let resp = self
            .client
            .post(&self.api_url)
            .header("api-key", &self.api_key)
            .header(reqwest::header::ACCEPT, "application/json")
            .json(&BrevoRequest {
                sender: &self.sender,
                to: [BrevoAddress { name: None, email: email.to.clone() }],
                subject: &email.subject,
                html_content: &email.html,
                text_content: &email.text,
            })
            .send()
            .await
            // reqwest's error text names the URL, never the headers.
            .map_err(|e| anyhow::anyhow!("brevo {}: {e}", self.api_url))?;
        let status = resp.status();
        if !status.is_success() {
            let body = resp.text().await.unwrap_or_default();
            anyhow::bail!("brevo {} returned {status}: {}", self.api_url, body.chars().take(300).collect::<String>());
        }
        Ok(())
    }
}

/// Prints emails (development).
pub struct StdoutMailer;

#[async_trait]
impl Mailer for StdoutMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        println!(
            "----- email (not sent) -----\nTo: {}\nSubject: {}\n\n{}\n----------------------------",
            email.to, email.subject, email.text
        );
        Ok(())
    }
}

/// Appends each email as one JSON line (development, process-level tests).
pub struct FileMailer {
    path: std::path::PathBuf,
    lock: tokio::sync::Mutex<()>,
}

impl FileMailer {
    pub fn new(path: impl Into<std::path::PathBuf>) -> Self {
        FileMailer { path: path.into(), lock: tokio::sync::Mutex::new(()) }
    }
}

#[async_trait]
impl Mailer for FileMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        use tokio::io::AsyncWriteExt;
        let _g = self.lock.lock().await;
        let mut f = tokio::fs::OpenOptions::new().create(true).append(true).open(&self.path).await?;
        let mut line = serde_json::to_string(email)?;
        line.push('\n');
        f.write_all(line.as_bytes()).await?;
        Ok(())
    }
}

/// Keeps emails in memory (tests).
#[derive(Default, Clone)]
pub struct MemoryMailer(Arc<Mutex<Vec<Email>>>);

impl MemoryMailer {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn sent(&self) -> Vec<Email> {
        self.0.lock().unwrap().clone()
    }
    /// The last email sent to `to`, if any.
    pub fn last_to(&self, to: &str) -> Option<Email> {
        self.0.lock().unwrap().iter().rev().find(|e| e.to == to).cloned()
    }
}

#[async_trait]
impl Mailer for MemoryMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        self.0.lock().unwrap().push(email.clone());
        Ok(())
    }
}

/// Wraps a mailer with a per-UTC-day cap so a bug or abuse cannot burn the
/// provider quota (Resend free tier: 100/day).
pub struct DailyCapMailer<M> {
    inner: M,
    limit: u32,
    state: Mutex<(NaiveDate, u32)>,
}

impl<M: Mailer> DailyCapMailer<M> {
    pub fn new(inner: M, limit: u32) -> Self {
        DailyCapMailer { inner, limit, state: Mutex::new((Utc::now().date_naive(), 0)) }
    }
}

#[async_trait]
impl<M: Mailer> Mailer for DailyCapMailer<M> {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        {
            let mut st = self.state.lock().unwrap();
            let today = Utc::now().date_naive();
            if st.0 != today {
                *st = (today, 0);
            }
            if st.1 >= self.limit {
                anyhow::bail!("daily email limit of {} reached", self.limit);
            }
            st.1 += 1;
        }
        self.inner.send(email).await
    }
}

/// Any SMTP submission server (Brevo, SMTP2GO, Mailjet, Postmark, ...), through
/// lettre with rustls. One connection per email (no pool): mail is rare here,
/// and a fresh connection never trips over one the server closed.
pub struct SmtpMailer {
    transport: AsyncSmtpTransport<Tokio1Executor>,
    from: Mailbox,
    /// `host:port (tls mode)`, for error messages. Never holds credentials.
    target: String,
    timeout: Duration,
}

fn is_loopback_host(host: &str) -> bool {
    let h = host.trim_start_matches('[').trim_end_matches(']');
    h.eq_ignore_ascii_case("localhost") || h.parse::<std::net::IpAddr>().is_ok_and(|ip| ip.is_loopback())
}

impl SmtpMailer {
    /// Checks the SMTP settings and builds the transport. Does not connect.
    pub fn new(cfg: &MailConfig) -> anyhow::Result<Self> {
        let host = cfg
            .smtp_host
            .as_deref()
            .map(str::trim)
            .filter(|h| !h.is_empty())
            .ok_or_else(|| anyhow::anyhow!("MAILER=smtp needs SMTP_HOST"))?;
        let port = cfg.smtp_port();
        let from: Mailbox = cfg.mail_from.parse().map_err(|e| {
            anyhow::anyhow!("MAIL_FROM {:?} is not a valid sender (`Name <address>` or `address`): {e}", cfg.mail_from)
        })?;
        let builder = match cfg.smtp_tls {
            SmtpTls::Starttls => AsyncSmtpTransport::<Tokio1Executor>::starttls_relay(host)
                .with_context(|| format!("SMTP_HOST {host:?}: TLS setup"))?,
            SmtpTls::Tls => AsyncSmtpTransport::<Tokio1Executor>::relay(host)
                .with_context(|| format!("SMTP_HOST {host:?}: TLS setup"))?,
            SmtpTls::None => {
                anyhow::ensure!(
                    is_loopback_host(host),
                    "SMTP_TLS=none is only allowed for a local test server (localhost, 127.0.0.1 or ::1), not {host:?}"
                );
                AsyncSmtpTransport::<Tokio1Executor>::builder_dangerous(host)
            }
        };
        let timeout = Duration::from_secs(cfg.smtp_timeout_secs.max(1));
        let mut builder = builder.port(port).timeout(Some(timeout));
        let username = cfg.smtp_username.as_deref().filter(|u| !u.is_empty());
        let password = cfg.smtp_password.as_ref().filter(|p| !p.is_empty());
        match (username, password) {
            (Some(u), Some(p)) => builder = builder.credentials(Credentials::new(u.into(), p.expose().into())),
            (None, None) => {}
            (Some(_), None) => anyhow::bail!("SMTP_USERNAME is set but SMTP_PASSWORD is empty"),
            (None, Some(_)) => anyhow::bail!("SMTP_PASSWORD is set but SMTP_USERNAME is empty"),
        }
        let tls = match cfg.smtp_tls {
            SmtpTls::Starttls => "starttls",
            SmtpTls::Tls => "tls",
            SmtpTls::None => "no tls",
        };
        Ok(SmtpMailer { transport: builder.build(), from, target: format!("{host}:{port} ({tls})"), timeout })
    }

    /// The MIME message: multipart/alternative with the text and HTML parts.
    pub fn message(&self, email: &Email) -> anyhow::Result<Message> {
        let to: Mailbox =
            email.to.parse().map_err(|e| anyhow::anyhow!("recipient {:?} is not a valid address: {e}", email.to))?;
        let message_id = format!("<{}@{}>", uuid::Uuid::new_v4().simple(), self.from.email.domain());
        Message::builder()
            .from(self.from.clone())
            .to(to)
            .subject(&email.subject)
            .message_id(Some(message_id))
            .multipart(MultiPart::alternative_plain_html(email.text.clone(), email.html.clone()))
            .context("building the email")
    }
}

#[async_trait]
impl Mailer for SmtpMailer {
    async fn send(&self, email: &Email) -> anyhow::Result<()> {
        let msg = self.message(email)?;
        match tokio::time::timeout(self.timeout, self.transport.send(msg)).await {
            Err(_) => anyhow::bail!("smtp {}: no answer within {} s", self.target, self.timeout.as_secs()),
            // lettre's Display already includes the server's reply and the cause.
            Ok(Err(e)) => anyhow::bail!("smtp {}: {e}", self.target),
            Ok(Ok(resp)) => {
                tracing::debug!(target_server = %self.target, code = %resp.code(), "smtp accepted the email");
                Ok(())
            }
        }
    }
}

/// Builds the mailer the config asks for. Checks the settings (missing host or
/// key, bad sender) but does not connect anywhere.
pub fn from_config(cfg: &MailConfig) -> anyhow::Result<Arc<dyn Mailer>> {
    Ok(match cfg.mailer_kind() {
        MailerKind::Resend => {
            let key = cfg
                .resend_api_key
                .as_ref()
                .filter(|k| !k.is_empty())
                .ok_or_else(|| anyhow::anyhow!("MAILER=resend needs RESEND_API_KEY"))?;
            Arc::new(DailyCapMailer::new(
                ResendMailer::new(&cfg.resend_api_url, key.expose(), &cfg.mail_from),
                cfg.mail_daily_limit,
            ))
        }
        MailerKind::Brevo => {
            let key = cfg
                .brevo_api_key
                .as_ref()
                .filter(|k| !k.is_empty())
                .ok_or_else(|| anyhow::anyhow!("MAILER=brevo needs BREVO_API_KEY"))?;
            Arc::new(DailyCapMailer::new(
                BrevoMailer::new(&cfg.brevo_api_url, key.expose(), &cfg.mail_from)?,
                cfg.mail_daily_limit,
            ))
        }
        MailerKind::Smtp => Arc::new(DailyCapMailer::new(SmtpMailer::new(cfg)?, cfg.mail_daily_limit)),
        MailerKind::Stdout => Arc::new(StdoutMailer),
        MailerKind::File => Arc::new(DailyCapMailer::new(FileMailer::new(&cfg.mail_file), cfg.mail_daily_limit)),
    })
}

/// One log-safe line about the mail settings (no credentials).
pub fn describe(cfg: &MailConfig) -> String {
    match cfg.mailer_kind() {
        MailerKind::Resend => format!("resend {}", cfg.resend_api_url),
        MailerKind::Brevo => format!("brevo {}", cfg.brevo_api_url),
        MailerKind::Smtp => format!(
            "smtp {}:{} tls={:?} auth={}",
            cfg.smtp_host.as_deref().unwrap_or("?"),
            cfg.smtp_port(),
            cfg.smtp_tls,
            cfg.smtp_username.as_deref().is_some_and(|u| !u.is_empty())
        ),
        MailerKind::Stdout => "stdout".into(),
        MailerKind::File => format!("file {}", cfg.mail_file),
    }
}

fn escape_html(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

pub fn verification_email(to: &str, display_name: &str, link: &str) -> Email {
    Email {
        to: to.into(),
        subject: format!("Verify your {PRODUCT_NAME} email"),
        text: format!(
            "Hi {display_name},\n\nOpen this link to verify the email address of your {PRODUCT_NAME} account:\n\n{link}\n\nThe link expires in 48 hours. If you did not sign up for {PRODUCT_NAME}, ignore this email.\n"
        ),
        html: format!(
            "<p>Hi {},</p><p>Open this link to verify the email address of your {p} account:</p><p><a href=\"{}\">{}</a></p><p>The link expires in 48 hours. If you did not sign up for {p}, ignore this email.</p>",
            escape_html(display_name),
            escape_html(link),
            escape_html(link),
            p = escape_html(PRODUCT_NAME)
        ),
    }
}

pub fn reset_email(to: &str, display_name: &str, link: &str) -> Email {
    Email {
        to: to.into(),
        subject: format!("Reset your {PRODUCT_NAME} password"),
        text: format!(
            "Hi {display_name},\n\nSomeone asked to reset the password for your {PRODUCT_NAME} account. Open this link to choose a new one:\n\n{link}\n\nThe link expires in 1 hour. If this was not you, ignore this email; your password has not changed.\n"
        ),
        html: format!(
            "<p>Hi {},</p><p>Someone asked to reset the password for your {p} account. Open this link to choose a new one:</p><p><a href=\"{}\">{}</a></p><p>The link expires in 1 hour. If this was not you, ignore this email; your password has not changed.</p>",
            escape_html(display_name),
            escape_html(link),
            escape_html(link),
            p = escape_html(PRODUCT_NAME)
        ),
    }
}

/// Pulls the `token=` value out of an email body (tests and dev tooling).
pub fn extract_token(text: &str) -> Option<String> {
    let start = text.find("token=")? + "token=".len();
    let token: String =
        text[start..].chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').collect();
    (!token.is_empty()).then_some(token)
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{extract::State, http::HeaderMap, routing::post, Json, Router};

    #[tokio::test]
    async fn resend_request_shape_against_fake_server() {
        // A local stand-in for api.resend.com that records what it receives.
        type Seen = Arc<Mutex<Vec<(Option<String>, serde_json::Value)>>>;
        let seen: Seen = Default::default();
        async fn handler(
            State(seen): State<Seen>,
            headers: HeaderMap,
            Json(body): Json<serde_json::Value>,
        ) -> Json<serde_json::Value> {
            let auth = headers.get("authorization").map(|v| v.to_str().unwrap().to_string());
            seen.lock().unwrap().push((auth, body));
            Json(serde_json::json!({"id": "fake"}))
        }
        let app = Router::new().route("/emails", post(handler)).with_state(seen.clone());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });

        let m = ResendMailer::new(&format!("http://{addr}/emails"), "re_test_key", "noreply@brawlonline.net");
        m.send(&verification_email("a@b.test", "Sarah", "http://x/verify-email?token=abc")).await.unwrap();

        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 1);
        assert_eq!(seen[0].0.as_deref(), Some("Bearer re_test_key"));
        let body = &seen[0].1;
        assert_eq!(body["from"], "noreply@brawlonline.net");
        assert_eq!(body["to"], serde_json::json!(["a@b.test"]));
        assert_eq!(body["subject"], "Verify your Brawl Online email");
        assert!(body["text"].as_str().unwrap().contains("token=abc"));
        assert!(body["html"].as_str().unwrap().contains("href="));
    }

    #[tokio::test]
    async fn resend_error_status_is_an_error() {
        let app = Router::new().route("/emails", post(|| async { (axum::http::StatusCode::UNAUTHORIZED, "bad key") }));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
        let m = ResendMailer::new(&format!("http://{addr}/emails"), "k", "f@x.test");
        let err = m.send(&reset_email("a@b.test", "x", "l")).await.unwrap_err();
        assert!(err.to_string().contains("401"));
    }

    /// A local stand-in for api.brevo.com: records (api-key header, body) and
    /// answers with `status`.
    async fn fake_brevo(
        status: axum::http::StatusCode,
    ) -> (std::net::SocketAddr, Arc<Mutex<Vec<(Option<String>, serde_json::Value)>>>) {
        type Seen = Arc<Mutex<Vec<(Option<String>, serde_json::Value)>>>;
        let seen: Seen = Default::default();
        let s2 = seen.clone();
        let app = Router::new().route(
            "/v3/smtp/email",
            post(move |headers: HeaderMap, Json(body): Json<serde_json::Value>| {
                let seen = s2.clone();
                async move {
                    let key = headers.get("api-key").map(|v| v.to_str().unwrap().to_string());
                    seen.lock().unwrap().push((key, body));
                    if status.is_success() {
                        (status, Json(serde_json::json!({"messageId": "<fake@smtp-relay.mailin.fr>"})))
                    } else {
                        (status, Json(serde_json::json!({"code": "unauthorized", "message": "Key not found"})))
                    }
                }
            }),
        );
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
        (addr, seen)
    }

    fn brevo_cfg(addr: std::net::SocketAddr) -> MailConfig {
        let mut c = MailConfig::for_tests();
        c.mailer = Some(MailerKind::Brevo);
        c.brevo_api_key = Some(Secret::new("xkeysib-test-key-must-not-leak"));
        c.brevo_api_url = format!("http://{addr}/v3/smtp/email");
        c.mail_from = "Brawl Online <noreply@brawlonline.test>".into();
        c
    }

    #[tokio::test]
    async fn brevo_request_shape_against_fake_server() {
        let (addr, seen) = fake_brevo(axum::http::StatusCode::CREATED).await;
        let m = from_config(&brevo_cfg(addr)).unwrap();
        let email = verification_email("a@b.test", "Sarah", "http://x/verify-email?token=abc");
        m.send(&email).await.unwrap();
        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 1);
        assert_eq!(seen[0].0.as_deref(), Some("xkeysib-test-key-must-not-leak"));
        let body = &seen[0].1;
        assert_eq!(body["sender"], serde_json::json!({"name": "Brawl Online", "email": "noreply@brawlonline.test"}));
        assert_eq!(body["to"], serde_json::json!([{"email": "a@b.test"}]));
        assert_eq!(body["subject"], "Verify your Brawl Online email");
        assert_eq!(body["textContent"], email.text);
        assert_eq!(body["htmlContent"], email.html);
    }

    #[tokio::test]
    async fn brevo_error_status_is_a_clear_error_without_the_key() {
        let (addr, _) = fake_brevo(axum::http::StatusCode::UNAUTHORIZED).await;
        let m = from_config(&brevo_cfg(addr)).unwrap();
        let err = m.send(&reset_email("a@b.test", "x", "l")).await.unwrap_err();
        let msg = format!("{err:#}");
        assert!(msg.starts_with(&format!("brevo http://{addr}/v3/smtp/email returned 401")), "{msg}");
        assert!(msg.contains("Key not found"), "{msg}");
        assert!(!msg.contains("xkeysib"), "{msg}");
        assert!(!describe(&brevo_cfg(addr)).contains("xkeysib"));
    }

    #[tokio::test]
    async fn brevo_unreachable_is_an_error_without_the_key() {
        let mut c = brevo_cfg("127.0.0.1:9".parse().unwrap());
        c.brevo_api_url = "http://127.0.0.1:9/v3/smtp/email".into();
        let err = from_config(&c).unwrap().send(&reset_email("a@b.test", "x", "l")).await.unwrap_err();
        let msg = format!("{err:#}");
        assert!(msg.starts_with("brevo http://127.0.0.1:9/v3/smtp/email: "), "{msg}");
        assert!(!msg.contains("xkeysib"), "{msg}");
    }

    #[test]
    fn brevo_settings_are_checked_up_front() {
        let mut c = brevo_cfg("127.0.0.1:9".parse().unwrap());
        c.brevo_api_key = None;
        assert!(from_config(&c).err().unwrap().to_string().contains("BREVO_API_KEY"));
        let mut c = brevo_cfg("127.0.0.1:9".parse().unwrap());
        c.mail_from = "nope".into();
        assert!(from_config(&c).err().unwrap().to_string().contains("MAIL_FROM"));
        // A key alone selects brevo when MAILER is unset.
        let mut c = brevo_cfg("127.0.0.1:9".parse().unwrap());
        c.mailer = None;
        assert_eq!(c.mailer_kind(), MailerKind::Brevo);
    }

    #[tokio::test]
    async fn daily_cap() {
        let mem = MemoryMailer::new();
        let capped = DailyCapMailer::new(mem.clone(), 2);
        let e = reset_email("a@b.test", "x", "l");
        capped.send(&e).await.unwrap();
        capped.send(&e).await.unwrap();
        assert!(capped.send(&e).await.is_err());
        assert_eq!(mem.sent().len(), 2);
    }

    #[tokio::test]
    async fn file_mailer_appends_json_lines() {
        let path = std::env::temp_dir().join(format!("pp-mail-{}.jsonl", uuid::Uuid::new_v4()));
        let m = FileMailer::new(&path);
        m.send(&verification_email("a@b.test", "x", "http://h/verify-email?token=t1")).await.unwrap();
        m.send(&verification_email("c@d.test", "y", "http://h/verify-email?token=t2")).await.unwrap();
        let text = std::fs::read_to_string(&path).unwrap();
        let lines: Vec<Email> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        assert_eq!(lines.len(), 2);
        assert_eq!(extract_token(&lines[1].text).as_deref(), Some("t2"));
        let _ = std::fs::remove_file(path);
    }

    // ---- SMTP, against the in-process fake server (crates/fakesmtp) ----

    use crate::config::Secret;
    use fakesmtp::{FakeSmtp, Mode};

    const SMTP_PASSWORD: &str = "smtp-pass-must-not-leak";

    fn smtp_cfg(port: u16) -> MailConfig {
        let mut c = MailConfig::for_tests();
        c.mailer = Some(MailerKind::Smtp);
        c.smtp_host = Some("127.0.0.1".into());
        c.smtp_port = Some(port);
        c.smtp_tls = SmtpTls::None;
        c.smtp_username = Some("smtp-user".into());
        c.smtp_password = Some(Secret::new(SMTP_PASSWORD));
        c.mail_from = "Brawl Online <noreply@brawlonline.test>".into();
        c
    }

    /// Undoes quoted-printable soft breaks and `=3D`, enough to search the body.
    fn unqp(s: &str) -> String {
        s.replace("=\r\n", "").replace("=3D", "=")
    }

    #[tokio::test]
    async fn smtp_sends_multipart_text_and_html() {
        let fake = FakeSmtp::start(Mode::Accept).await.unwrap();
        let m = from_config(&smtp_cfg(fake.port())).unwrap();
        let email = verification_email("alice@example.test", "Alice", "http://x/verify-email?token=tok123");
        m.send(&email).await.unwrap();

        let msgs = fake.wait_for_messages(1, Duration::from_secs(5)).await;
        assert_eq!(msgs.len(), 1);
        let s = &msgs[0];
        assert_eq!(s.auth, Some(("smtp-user".to_string(), SMTP_PASSWORD.to_string())));
        assert_eq!(s.mail_from.as_deref(), Some("noreply@brawlonline.test"));
        assert_eq!(s.rcpt_to, vec!["alice@example.test".to_string()]);
        let data = unqp(s.data.as_deref().unwrap());
        let header = |name: &str| {
            data.lines().find(|l| l.starts_with(name)).unwrap_or_else(|| panic!("no {name} in\n{data}")).to_string()
        };
        assert!(header("From: ").contains("Brawl Online") && header("From: ").contains("<noreply@brawlonline.test>"));
        assert_eq!(header("To: "), "To: alice@example.test");
        assert_eq!(header("Subject: "), "Subject: Verify your Brawl Online email");
        assert!(header("Message-ID: ").ends_with("@brawlonline.test>"));
        assert!(header("Date: ").len() > 10);
        assert!(data.contains("multipart/alternative"));
        assert!(data.contains("Content-Type: text/plain; charset=utf-8"));
        assert!(data.contains("Content-Type: text/html; charset=utf-8"));
        // Same content as the Resend mailer sends: every text line and the whole HTML.
        for line in email.text.lines().filter(|l| !l.is_empty()) {
            assert!(data.contains(line), "text line {line:?} missing from\n{data}");
        }
        assert!(data.contains(&email.html), "html missing from\n{data}");
        assert!(s.quit, "the client should end with QUIT");
    }

    #[tokio::test]
    async fn smtp_auth_failure_is_a_clear_error_without_the_password() {
        let fake = FakeSmtp::start(Mode::RejectAuth).await.unwrap();
        let m = from_config(&smtp_cfg(fake.port())).unwrap();
        let err = m.send(&reset_email("a@b.test", "x", "http://x/reset-password?token=t")).await.unwrap_err();
        let msg = format!("{err:#}");
        assert!(msg.starts_with(&format!("smtp 127.0.0.1:{} (no tls)", fake.port())), "{msg}");
        assert!(msg.contains("535"), "{msg}");
        assert!(msg.contains("Authentication credentials invalid"), "{msg}");
        assert!(!msg.contains(SMTP_PASSWORD), "{msg}");
        assert!(fake.messages().is_empty());
    }

    #[tokio::test]
    async fn smtp_times_out_on_a_silent_server() {
        let fake = FakeSmtp::start(Mode::Silent).await.unwrap();
        let mut cfg = smtp_cfg(fake.port());
        cfg.smtp_timeout_secs = 1;
        let m = from_config(&cfg).unwrap();
        let t0 = std::time::Instant::now();
        let err = m.send(&reset_email("a@b.test", "x", "l")).await.unwrap_err();
        assert!(t0.elapsed() < Duration::from_secs(5), "took {:?}", t0.elapsed());
        let msg = format!("{err:#}");
        assert!(msg.starts_with(&format!("smtp 127.0.0.1:{}", fake.port())), "{msg}");
        assert_eq!(fake.connections(), 1);
    }

    #[tokio::test]
    async fn starttls_is_required_so_credentials_never_go_out_in_plain_text() {
        // The fake server does not offer STARTTLS; SMTP_TLS=starttls must give up
        // before AUTH, MAIL or DATA.
        let fake = FakeSmtp::start(Mode::Accept).await.unwrap();
        let mut cfg = smtp_cfg(fake.port());
        cfg.smtp_tls = SmtpTls::Starttls;
        let m = from_config(&cfg).unwrap();
        let err = m.send(&reset_email("a@b.test", "x", "l")).await.unwrap_err();
        assert!(format!("{err:#}").contains("(starttls)"), "{err:#}");
        let sessions = fake.sessions();
        assert_eq!(sessions.len(), 1);
        assert!(sessions[0].ehlo.is_some());
        assert!(sessions[0].auth.is_none() && sessions[0].mail_from.is_none() && sessions[0].data.is_none());
    }

    #[test]
    fn smtp_settings_are_checked_up_front() {
        let err = |c: &MailConfig| from_config(c).err().map(|e| format!("{e:#}")).unwrap_or_default();
        let mut c = smtp_cfg(2525);
        c.smtp_host = None;
        assert!(err(&c).contains("SMTP_HOST"));
        let mut c = smtp_cfg(2525);
        c.smtp_host = Some("smtp.example.com".into());
        assert!(err(&c).contains("SMTP_TLS=none"), "plain SMTP to a remote host must be refused");
        let mut c = smtp_cfg(2525);
        c.smtp_password = None;
        assert!(err(&c).contains("SMTP_PASSWORD"));
        let mut c = smtp_cfg(2525);
        c.smtp_username = Some(String::new());
        assert!(err(&c).contains("SMTP_USERNAME"));
        let mut c = smtp_cfg(2525);
        c.mail_from = "not an address".into();
        assert!(err(&c).contains("MAIL_FROM"));
        // Valid remote settings build without connecting.
        let mut c = smtp_cfg(587);
        c.smtp_host = Some("smtp.example.com".into());
        c.smtp_tls = SmtpTls::Starttls;
        assert!(from_config(&c).is_ok());
        c.smtp_tls = SmtpTls::Tls;
        assert!(from_config(&c).is_ok());
        assert!(!describe(&c).contains(SMTP_PASSWORD));
        assert!(!describe(&c).contains("smtp-user"));
    }

    #[tokio::test]
    async fn local_mailers_never_touch_the_network() {
        // Every network setting points at a listener that counts connections.
        let tripwire = FakeSmtp::start(Mode::Accept).await.unwrap();
        let path = std::env::temp_dir().join(format!("pp-mail-{}.jsonl", uuid::Uuid::new_v4()));
        for kind in [MailerKind::Stdout, MailerKind::File] {
            let mut c = smtp_cfg(tripwire.port());
            c.mailer = Some(kind);
            c.resend_api_key = Some(Secret::new("re_must_not_be_used"));
            c.resend_api_url = format!("http://{}/emails", tripwire.addr);
            c.brevo_api_key = Some(Secret::new("xkeysib-must-not-be-used"));
            c.brevo_api_url = format!("http://{}/v3/smtp/email", tripwire.addr);
            c.mail_file = path.to_string_lossy().into_owned();
            from_config(&c).unwrap().send(&reset_email("a@b.test", "x", "http://x/?token=t")).await.unwrap();
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
        assert_eq!(tripwire.connections(), 0);
        assert_eq!(std::fs::read_to_string(&path).unwrap().lines().count(), 1);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn html_is_escaped() {
        let e = verification_email("a@b.test", "<script>", "http://x/?token=a&b");
        assert!(!e.html.contains("<script>"));
        assert!(e.html.contains("&amp;b"));
    }
}
