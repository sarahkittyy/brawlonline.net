//! Outgoing email: verification links and password resets.
//!
//! Production sends through Resend's HTTP API (`POST /emails`, Bearer key).
//! Development prints to stdout or appends JSON lines to a file, and tests use
//! [`MemoryMailer`]. Nothing in the test suite talks to the real API.

use std::sync::{Arc, Mutex};

use async_trait::async_trait;
use chrono::{NaiveDate, Utc};
use common::PRODUCT_NAME;
use serde::{Deserialize, Serialize};

use crate::config::{Config, MailerKind};

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

/// Builds the mailer the config asks for.
pub fn from_config(cfg: &Config) -> anyhow::Result<Arc<dyn Mailer>> {
    Ok(match cfg.mailer_kind() {
        MailerKind::Resend => {
            let key = cfg
                .resend_api_key
                .as_deref()
                .filter(|k| !k.is_empty())
                .ok_or_else(|| anyhow::anyhow!("MAILER=resend needs RESEND_API_KEY"))?;
            Arc::new(DailyCapMailer::new(
                ResendMailer::new(&cfg.resend_api_url, key, &cfg.mail_from),
                cfg.mail_daily_limit,
            ))
        }
        MailerKind::Stdout => Arc::new(StdoutMailer),
        MailerKind::File => Arc::new(DailyCapMailer::new(FileMailer::new(&cfg.mail_file), cfg.mail_daily_limit)),
    })
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

        let m = ResendMailer::new(&format!("http://{addr}/emails"), "re_test_key", "noreply@fluffycat.gay");
        m.send(&verification_email("a@b.test", "Sarah", "http://x/verify-email?token=abc")).await.unwrap();

        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 1);
        assert_eq!(seen[0].0.as_deref(), Some("Bearer re_test_key"));
        let body = &seen[0].1;
        assert_eq!(body["from"], "noreply@fluffycat.gay");
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

    #[test]
    fn html_is_escaped() {
        let e = verification_email("a@b.test", "<script>", "http://x/?token=a&b");
        assert!(!e.html.contains("<script>"));
        assert!(e.html.contains("&amp;b"));
    }
}
