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

/// A bug report as `admin bug list` / `admin bug show` print it (`migrations/0011_bug_reports.sql`).
#[derive(Debug, Clone, sqlx::FromRow)]
pub struct BugReportRow {
    pub id: i64,
    pub created_at: chrono::DateTime<chrono::Utc>,
    pub uid: Option<uuid::Uuid>,
    pub connect_code: Option<String>,
    pub display_name: Option<String>,
    pub description: String,
    pub launcher_version: String,
    pub platform: String,
    /// The zip's size, or `None` when no zip came (yet).
    pub logs_bytes: Option<i64>,
    pub resolved_at: Option<chrono::DateTime<chrono::Utc>>,
    pub resolution: String,
}

const BUG_REPORT_COLUMNS: &str = "b.id, b.created_at, b.uid, u.connect_code, u.display_name, b.description,
    b.launcher_version, b.platform, b.logs_bytes, b.resolved_at, b.resolution
    FROM bug_reports b LEFT JOIN users u ON u.uid = b.uid";

/// Newest first: the unresolved ones, or with `all` the resolved ones too.
pub async fn bug_reports(pool: &PgPool, all: bool, limit: i64) -> anyhow::Result<Vec<BugReportRow>> {
    Ok(sqlx::query_as::<_, BugReportRow>(&format!(
        "SELECT {BUG_REPORT_COLUMNS} WHERE $1 OR b.resolved_at IS NULL ORDER BY b.id DESC LIMIT $2"
    ))
    .bind(all)
    .bind(limit)
    .fetch_all(pool)
    .await?)
}

pub async fn bug_report(pool: &PgPool, id: i64) -> anyhow::Result<BugReportRow> {
    sqlx::query_as::<_, BugReportRow>(&format!("SELECT {BUG_REPORT_COLUMNS} WHERE b.id = $1"))
        .bind(id)
        .fetch_optional(pool)
        .await?
        .ok_or_else(|| anyhow::anyhow!("no bug report #{id}"))
}

/// Marks a report resolved, with an optional note (it leaves `bug list`). `false` if there is no
/// such unresolved report.
pub async fn resolve_bug_report(pool: &PgPool, actor: &str, id: i64, note: &str) -> anyhow::Result<bool> {
    let done = sqlx::query(
        "UPDATE bug_reports SET resolved_at = now(), resolution = $2 WHERE id = $1 AND resolved_at IS NULL",
    )
    .bind(id)
    .bind(note)
    .execute(pool)
    .await?
    .rows_affected()
        == 1;
    if done {
        store::audit(pool, actor, "bug_report.resolve", Some(&id.to_string()), serde_json::json!({"note": note}))
            .await?;
    }
    Ok(done)
}

/// Puts a resolved report back on the list. `false` if there is no such resolved report.
pub async fn reopen_bug_report(pool: &PgPool, actor: &str, id: i64) -> anyhow::Result<bool> {
    let done = sqlx::query("UPDATE bug_reports SET resolved_at = NULL WHERE id = $1 AND resolved_at IS NOT NULL")
        .bind(id)
        .execute(pool)
        .await?
        .rows_affected()
        == 1;
    if done {
        store::audit(pool, actor, "bug_report.reopen", Some(&id.to_string()), serde_json::json!({})).await?;
    }
    Ok(done)
}

/// Deletes reports and their zips in `dir`: the ids given, or with `ids` empty every resolved
/// report. The zips go first, so a report is never left pointing at nothing it claims to have.
/// Returns the ids deleted.
pub async fn delete_bug_reports(
    pool: &PgPool,
    actor: &str,
    dir: &std::path::Path,
    ids: &[i64],
) -> anyhow::Result<Vec<i64>> {
    let found: Vec<i64> = if ids.is_empty() {
        sqlx::query_scalar("SELECT id FROM bug_reports WHERE resolved_at IS NOT NULL ORDER BY id")
            .fetch_all(pool)
            .await?
    } else {
        sqlx::query_scalar("SELECT id FROM bug_reports WHERE id = ANY($1) ORDER BY id")
            .bind(ids)
            .fetch_all(pool)
            .await?
    };
    for &id in &found {
        let path = accounts::bug_reports::logs_path(dir, id);
        match std::fs::remove_file(&path) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => anyhow::bail!("deleting {}: {e}", path.display()),
        }
        sqlx::query("DELETE FROM bug_reports WHERE id = $1").bind(id).execute(pool).await?;
    }
    if !found.is_empty() {
        store::audit(pool, actor, "bug_report.delete", None, serde_json::json!({"ids": found})).await?;
    }
    Ok(found)
}
