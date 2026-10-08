//! `admin user reset-password --send-email` uses the mailer the MAIL settings
//! select, exactly as `accounts` does, and the local mailers never open a
//! network connection.
//!
//! The binary tests run the real `admin` executable against a fresh database,
//! with every network setting (RESEND_API_URL with a key, SMTP_HOST/PORT)
//! pointing at a local listener that counts connections. Before the fix the
//! admin CLI ignored MAILER and always called Resend, which these tests catch.

use std::path::{Path, PathBuf};
use std::process::Output;
use std::time::Duration;

use accounts::mail::{extract_token, Email, MemoryMailer};
use accounts::store::{self, TokenPurpose};
use fakesmtp::{FakeSmtp, Mode};
use sqlx::PgPool;

const EMAIL: &str = "zz-admin-mail@example.test";
const BASE: &str = "https://brawlonline.test";

struct Db {
    pool: PgPool,
    name: String,
    url: String,
    dir: PathBuf,
}

async fn setup() -> Db {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    store::create_user(&pool, EMAIL, "not-a-real-hash", "MailTest", None).await.unwrap();
    // A working directory without a .env, for the mail file.
    let dir = std::env::temp_dir().join(format!("pp-admin-mail-{name}"));
    std::fs::create_dir_all(&dir).unwrap();
    Db { pool, name, url, dir }
}

impl Db {
    async fn finish(self) {
        self.pool.close().await;
        e2e::drop_db(&self.name).await;
        let _ = std::fs::remove_dir_all(&self.dir);
    }

    async fn reset_tokens(&self) -> i64 {
        sqlx::query_scalar("SELECT count(*) FROM email_tokens WHERE purpose = 'reset_password'")
            .fetch_one(&self.pool)
            .await
            .unwrap()
    }

    async fn last_audit(&self) -> serde_json::Value {
        sqlx::query_scalar("SELECT detail FROM audit_log WHERE action = 'user.reset_password' ORDER BY id DESC LIMIT 1")
            .fetch_one(&self.pool)
            .await
            .unwrap()
    }
}

/// Runs `admin user reset-password EMAIL --send-email` with MAILER=`mailer`.
/// Resend has a key and points at `resend`, SMTP points at `smtp`.
async fn run_admin(db: &Db, mailer: &str, resend: &FakeSmtp, smtp: &FakeSmtp, resend_key: &str) -> Output {
    let out = tokio::process::Command::new(env!("CARGO_BIN_EXE_admin"))
        .args(["user", "reset-password", EMAIL, "--send-email"])
        .current_dir(&db.dir)
        .env("DATABASE_URL", &db.url)
        .env("PUBLIC_BASE_URL", BASE)
        .env("MAILER", mailer)
        .env("RESEND_API_KEY", resend_key)
        .env("RESEND_API_URL", format!("http://{}/emails", resend.addr))
        .env("SMTP_HOST", "127.0.0.1")
        .env("SMTP_PORT", smtp.port().to_string())
        .env("SMTP_TLS", "none")
        .env("SMTP_USERNAME", "smtp-user")
        .env("SMTP_PASSWORD", "smtp-pass")
        .env("SMTP_TIMEOUT_SECS", "5")
        .env("MAIL_FROM", "Brawl Online <noreply@brawlonline.test>")
        .env("MAIL_FILE", db.dir.join("mail.jsonl"))
        .output()
        .await
        .unwrap();
    eprintln!(
        "admin MAILER={mailer}: {}\nstdout:\n{}\nstderr:\n{}",
        out.status,
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    out
}

fn mail_file(dir: &Path) -> Vec<Email> {
    std::fs::read_to_string(dir.join("mail.jsonl"))
        .unwrap_or_default()
        .lines()
        .map(|l| serde_json::from_str(l).unwrap())
        .collect()
}

async fn tripwires() -> (FakeSmtp, FakeSmtp) {
    (FakeSmtp::start(Mode::Accept).await.unwrap(), FakeSmtp::start(Mode::Accept).await.unwrap())
}

#[tokio::test]
async fn file_mailer_writes_the_mail_file_and_never_connects() {
    let db = setup().await;
    let (resend, smtp) = tripwires().await;
    let out = run_admin(&db, "file", &resend, &smtp, "re_must_not_be_used").await;
    assert!(out.status.success());
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert_eq!(resend.connections(), 0, "admin called the Resend URL with MAILER=file");
    assert_eq!(smtp.connections(), 0, "admin connected to SMTP with MAILER=file");

    let mails = mail_file(&db.dir);
    assert_eq!(mails.len(), 1);
    assert_eq!(mails[0].to, EMAIL);
    assert_eq!(mails[0].subject, "Reset your Brawl Online password");
    assert!(mails[0].text.contains(&format!("{BASE}/reset-password?token=")));
    let token = extract_token(&mails[0].text).unwrap();
    assert!(store::peek_email_token(&db.pool, &token, TokenPurpose::ResetPassword).await.unwrap().is_some());
    assert!(String::from_utf8_lossy(&out.stdout).contains("(mailer: file "));
    let audit = db.last_audit().await;
    assert_eq!(audit["emailed"], true);
    assert_eq!(audit["mailer"], "file");
    db.finish().await;
}

#[tokio::test]
async fn stdout_mailer_prints_and_never_connects() {
    let db = setup().await;
    let (resend, smtp) = tripwires().await;
    let out = run_admin(&db, "stdout", &resend, &smtp, "re_must_not_be_used").await;
    assert!(out.status.success());
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert_eq!(resend.connections(), 0, "admin called the Resend URL with MAILER=stdout");
    assert_eq!(smtp.connections(), 0, "admin connected to SMTP with MAILER=stdout");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(stdout.contains("----- email (not sent) -----"));
    assert!(stdout.contains(&format!("To: {EMAIL}")));
    assert!(stdout.contains(&format!("{BASE}/reset-password?token=")));
    assert!(mail_file(&db.dir).is_empty());
    db.finish().await;
}

#[tokio::test]
async fn memory_mailer_through_the_admin_path() {
    // The CLI's reset path sends through the mailer it is given and builds no
    // other: with a MemoryMailer nothing can leave the process.
    let db = setup().await;
    let mem = MemoryMailer::new();
    let r = admin::reset_password(&db.pool, "admin-cli:test", EMAIL, BASE, Some((&mem, "memory"))).await.unwrap();
    let sent = mem.sent();
    assert_eq!(sent.len(), 1);
    assert_eq!(sent[0].to, EMAIL);
    assert!(sent[0].text.contains(&r.link));
    assert!(sent[0].html.contains("reset-password?token="));
    assert_eq!(db.last_audit().await["mailer"], "memory");
    // Without a mailer, only the link is returned.
    let r2 = admin::reset_password(&db.pool, "admin-cli:test", EMAIL, BASE, None).await.unwrap();
    assert_ne!(r.link, r2.link);
    assert_eq!(mem.sent().len(), 1);
    assert_eq!(db.last_audit().await["emailed"], false);
    db.finish().await;
}

#[tokio::test]
async fn smtp_mailer_sends_through_the_configured_server() {
    let db = setup().await;
    let (resend, smtp) = tripwires().await;
    let out = run_admin(&db, "smtp", &resend, &smtp, "re_must_not_be_used").await;
    assert!(out.status.success());
    let msgs = smtp.wait_for_messages(1, Duration::from_secs(5)).await;
    assert_eq!(msgs.len(), 1);
    assert_eq!(msgs[0].rcpt_to, vec![EMAIL.to_string()]);
    assert_eq!(msgs[0].auth, Some(("smtp-user".to_string(), "smtp-pass".to_string())));
    assert!(msgs[0].data.as_deref().unwrap().contains("Subject: Reset your Brawl Online password"));
    assert_eq!(resend.connections(), 0);
    assert!(!String::from_utf8_lossy(&out.stdout).contains("smtp-pass"));
    db.finish().await;
}

#[tokio::test]
async fn resend_without_a_key_fails_before_issuing_a_token() {
    // The production incident: MAILER unset/resend with an empty key. Now it
    // is a configuration error, nothing is sent and no link is issued.
    let db = setup().await;
    let (resend, smtp) = tripwires().await;
    let out = run_admin(&db, "resend", &resend, &smtp, "").await;
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("RESEND_API_KEY"));
    assert_eq!(resend.connections(), 0);
    assert_eq!(smtp.connections(), 0);
    assert_eq!(db.reset_tokens().await, 0);
    db.finish().await;
}
