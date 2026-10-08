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
