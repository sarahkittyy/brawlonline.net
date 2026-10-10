//! The parts of the admin CLI that tests call directly.

use accounts::mail::{self, Mailer};
use accounts::store::{self, TokenPurpose, UserRow};
use chrono::Duration;
use sqlx::PgPool;

pub async fn find(pool: &PgPool, ident: &str) -> anyhow::Result<UserRow> {
    store::find_user(pool, ident).await?.ok_or_else(|| anyhow::anyhow!("no user matches {ident:?}"))
}

/// A one-time reset link for one account.
#[derive(Debug, Clone)]
pub struct ResetLink {
    pub email: String,
    pub link: String,
}

/// Issues a password-reset link valid 24 h. With `mailer`, also sends it with
/// [`mail::reset_email`] through that mailer, which the CLI builds from the
/// same settings as `accounts` ([`mail::from_config`]). The audit row says
/// whether it was emailed and with which mailer.
pub async fn reset_password(
    pool: &PgPool,
    actor: &str,
    ident: &str,
    public_base_url: &str,
    mailer: Option<(&dyn Mailer, &str)>,
) -> anyhow::Result<ResetLink> {
    let u = find(pool, ident).await?;
    let token = store::create_email_token(pool, u.uid, TokenPurpose::ResetPassword, Duration::hours(24)).await?;
    let link = format!("{}/reset-password?token={token}", public_base_url.trim_end_matches('/'));
    let sent = match mailer {
        Some((m, _)) => Some(m.send(&mail::reset_email(&u.email, &u.display_name, &link)).await),
        None => None,
    };
    store::audit(
        pool,
        actor,
        "user.reset_password",
        Some(&u.uid.to_string()),
        serde_json::json!({
            "emailed": matches!(sent, Some(Ok(()))),
            "mailer": mailer.map(|(_, kind)| kind),
        }),
    )
    .await?;
    if let Some(Err(e)) = sent {
        return Err(e.context(format!("emailing the reset link to {}", u.email)));
    }
    Ok(ResetLink { email: u.email, link })
}

/// A chat report as `admin chat-reports` lists it (`migrations/0006_chat.sql`).
#[derive(Debug, Clone, sqlx::FromRow)]
pub struct ChatReportRow {
    pub id: i64,
    pub created_at: chrono::DateTime<chrono::Utc>,
    pub reporter: Option<uuid::Uuid>,
    pub reporter_code: Option<String>,
    pub reporter_name: Option<String>,
    pub reported: Option<uuid::Uuid>,
    pub reported_code: Option<String>,
    pub reported_name: Option<String>,
    pub group_id: String,
    pub reason: String,
    /// `[{seq, text, raw, sig, verified}]`.
    pub messages: serde_json::Value,
    pub verified: i32,
    pub total: i32,
}

/// The reports nobody has handled yet, newest first.
pub async fn unhandled_chat_reports(pool: &PgPool, limit: i64) -> anyhow::Result<Vec<ChatReportRow>> {
    Ok(sqlx::query_as::<_, ChatReportRow>(
        "SELECT r.id, r.created_at, r.reporter, a.connect_code AS reporter_code, a.display_name AS reporter_name,
                r.reported, b.connect_code AS reported_code, b.display_name AS reported_name,
                r.group_id, r.reason, r.messages, r.verified, r.total
           FROM chat_reports r
           LEFT JOIN users a ON a.uid = r.reporter
           LEFT JOIN users b ON b.uid = r.reported
          WHERE r.handled_at IS NULL
          ORDER BY r.created_at DESC, r.id DESC
          LIMIT $1",
    )
    .bind(limit)
    .fetch_all(pool)
    .await?)
}

/// Marks a report handled (it leaves the list). `false` if there is no such unhandled report.
pub async fn chat_report_done(pool: &PgPool, actor: &str, id: i64) -> anyhow::Result<bool> {
    let done = sqlx::query("UPDATE chat_reports SET handled_at = now() WHERE id = $1 AND handled_at IS NULL")
        .bind(id)
        .execute(pool)
        .await?
        .rows_affected()
        == 1;
    if done {
        store::audit(pool, actor, "chat_report.done", Some(&id.to_string()), serde_json::json!({})).await?;
    }
    Ok(done)
}
