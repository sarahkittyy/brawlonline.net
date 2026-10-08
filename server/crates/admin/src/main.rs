//! `admin`: operator CLI for the accounts database. Every change is written to
//! `audit_log`. Reads DATABASE_URL (and, for `reset-password --send-email`,
//! the mail settings) from the environment or `server/.env`.

use std::time::Duration as StdDuration;

use accounts::config::MailConfig;
use accounts::mail;
use accounts::store::{self, UserRow};
use chrono::{Duration, Utc};
use clap::{Parser, Subcommand};
use sqlx::PgPool;

#[derive(Parser)]
#[command(name = "admin", version, about = "Admin CLI: invites, users, password resets, bans")]
struct Cli {
    #[arg(long, env = "DATABASE_URL", global = true, hide_env_values = true)]
    database_url: Option<String>,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Apply database migrations.
    Migrate,
    /// Print a new random PLAY_KEY_SECRET.
    GenSecret,
    /// Invite codes for sign-up.
    #[command(subcommand)]
    Invite(InviteCmd),
    /// Accounts. IDENT is a uid, an email or a connect code.
    #[command(subcommand)]
    User(UserCmd),
}

#[derive(Subcommand)]
enum InviteCmd {
    /// Create an invite code.
    Create {
        #[arg(long, default_value_t = 1)]
        uses: i32,
        #[arg(long)]
        expires_days: Option<i64>,
        #[arg(long)]
        note: Option<String>,
    },
    List,
    Revoke {
        code: String,
    },
}

#[derive(Subcommand)]
enum UserCmd {
    List,
    Show {
        ident: String,
    },
    /// Issue a one-time password-reset link (valid 24 h). Prints it, or with --send-email
    /// sends it through the mailer the MAILER/SMTP_*/MAIL_* settings select, as accounts does.
    ResetPassword {
        ident: String,
        #[arg(long)]
        send_email: bool,
        #[arg(long, env = "PUBLIC_BASE_URL", default_value = "http://127.0.0.1:8080")]
        public_base_url: String,
        #[command(flatten)]
        mail: Box<MailConfig>,
    },
    /// Ban an account (permanent unless --days). Ends its sessions and rotates its play key.
    Ban {
        ident: String,
        #[arg(long)]
        days: Option<i64>,
        #[arg(long)]
        reason: Option<String>,
    },
    Unban {
        ident: String,
    },
    /// Mark the email verified (when mail delivery failed).
    VerifyEmail {
        ident: String,
    },
    /// Invalidate the current play key (the launcher fetches the new one).
    RotatePlayKey {
        ident: String,
    },
    /// Change the connect code (the one admin-approved change).
    SetCode {
        ident: String,
        code: String,
    },
}

fn actor() -> String {
    let who = std::env::var("USER").or_else(|_| std::env::var("USERNAME")).unwrap_or_else(|_| "unknown".into());
    format!("admin-cli:{who}")
}

async fn find(pool: &PgPool, ident: &str) -> anyhow::Result<UserRow> {
    admin::find(pool, ident).await
}

fn print_user(u: &UserRow) {
    println!("uid:            {}", u.uid);
    println!("email:          {} ({})", u.email, if u.email_verified() { "verified" } else { "not verified" });
    println!("display name:   {}", u.display_name);
    println!("connect code:   {}", u.connect_code.as_deref().unwrap_or("-"));
    println!("role:           {}", u.role);
    println!("play key ver.:  {}", u.play_key_version);
    println!("rules version:  {}", u.rules_version);
    println!("created:        {}", u.created_at.format("%Y-%m-%d %H:%M UTC"));
    match (&u.banned_until, u.is_banned()) {
        (Some(t), true) if *t >= store::permanent_ban() => {
            println!("banned:         permanently ({})", u.ban_reason.as_deref().unwrap_or(""))
        }
        (Some(t), true) => println!(
            "banned:         until {} ({})",
            t.format("%Y-%m-%d %H:%M UTC"),
            u.ban_reason.as_deref().unwrap_or("")
        ),
        _ => println!("banned:         no"),
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let _ = dotenvy::dotenv();
    let cli = Cli::parse();
    if let Cmd::GenSecret = cli.cmd {
        println!("{}", common::playkey::PlayKeySecret::generate_hex());
        return Ok(());
    }
    let url = cli.database_url.ok_or_else(|| anyhow::anyhow!("DATABASE_URL is not set"))?;
    let pool = sqlx::postgres::PgPoolOptions::new()
        .max_connections(2)
        .acquire_timeout(StdDuration::from_secs(5))
        .connect(&url)
        .await?;
    let actor = actor();

    match cli.cmd {
        Cmd::GenSecret => unreachable!(),
        Cmd::Migrate => {
            common::db::MIGRATOR.run(&pool).await?;
            println!("migrations applied");
        }
        Cmd::Invite(InviteCmd::Create { uses, expires_days, note }) => {
            anyhow::ensure!(uses > 0, "--uses must be at least 1");
            let expires = expires_days.map(|d| Utc::now() + Duration::days(d));
            let inv = store::create_invite(&pool, uses, expires, note.as_deref(), &actor).await?;
            store::audit(
                &pool,
                &actor,
                "invite.create",
                Some(&inv.code),
                serde_json::json!({"uses": uses, "expires": expires, "note": note}),
            )
            .await?;
            println!("{}", inv.code);
        }
        Cmd::Invite(InviteCmd::List) => {
            println!("{:<20} {:>9} {:<17} {:<8} note", "code", "uses", "expires", "state");
            for i in store::list_invites(&pool).await? {
                let state = if i.revoked_at.is_some() {
                    "revoked"
                } else if i.expires_at.is_some_and(|t| t <= Utc::now()) {
                    "expired"
                } else if i.uses >= i.max_uses {
                    "used"
                } else {
                    "open"
                };
                let expires =
                    i.expires_at.map(|t| t.format("%Y-%m-%d %H:%M").to_string()).unwrap_or_else(|| "never".into());
                println!(
                    "{:<20} {:>4}/{:<4} {:<17} {:<8} {}",
                    i.code,
                    i.uses,
                    i.max_uses,
                    expires,
                    state,
                    i.note.unwrap_or_default()
                );
            }
        }
        Cmd::Invite(InviteCmd::Revoke { code }) => {
            anyhow::ensure!(store::revoke_invite(&pool, &code).await?, "no open invite {code}");
            store::audit(&pool, &actor, "invite.revoke", Some(&code), serde_json::json!({})).await?;
            println!("revoked");
        }
        Cmd::User(UserCmd::List) => {
            println!("{:<36} {:<10} {:<16} {:<30} {:<5} banned", "uid", "code", "name", "email", "ver.");
            for u in store::list_users(&pool).await? {
                println!(
                    "{:<36} {:<10} {:<16} {:<30} {:<5} {}",
                    u.uid,
                    u.connect_code.as_deref().unwrap_or("-"),
                    u.display_name,
                    u.email,
                    if u.email_verified() { "yes" } else { "no" },
                    if u.is_banned() { "yes" } else { "" }
                );
            }
        }
        Cmd::User(UserCmd::Show { ident }) => print_user(&find(&pool, &ident).await?),
        Cmd::User(UserCmd::ResetPassword { ident, send_email, public_base_url, mail: mail_cfg }) => {
            // The same mailer accounts would build from these settings (MAILER=file,
            // stdout, smtp or resend). Built first, so a bad setting fails before a
            // token is issued.
            let mailer = if send_email { Some(mail::from_config(&mail_cfg)?) } else { None };
            let kind = format!("{:?}", mail_cfg.mailer_kind()).to_lowercase();
            let r = admin::reset_password(
                &pool,
                &actor,
                &ident,
                &public_base_url,
                mailer.as_deref().map(|m| (m, kind.as_str())),
            )
            .await?;
            if send_email {
                println!("reset link for {} sent (mailer: {}; valid 24 h)", r.email, mail::describe(&mail_cfg));
            } else {
                println!("one-time reset link for {} (valid 24 h):\n{}", r.email, r.link);
            }
        }
        Cmd::User(UserCmd::Ban { ident, days, reason }) => {
            let u = find(&pool, &ident).await?;
            let until = days.map(|d| Utc::now() + Duration::days(d)).unwrap_or_else(store::permanent_ban);
            store::set_ban(&pool, u.uid, Some(until), reason.as_deref()).await?;
            store::audit(
                &pool,
                &actor,
                "user.ban",
                Some(&u.uid.to_string()),
                serde_json::json!({"until": until, "reason": reason}),
            )
            .await?;
            println!(
                "banned {} ({})",
                u.email,
                if days.is_some() {
                    format!("until {}", until.format("%Y-%m-%d %H:%M UTC"))
                } else {
                    "permanently".into()
                }
            );
        }
        Cmd::User(UserCmd::Unban { ident }) => {
            let u = find(&pool, &ident).await?;
            store::set_ban(&pool, u.uid, None, None).await?;
            store::audit(&pool, &actor, "user.unban", Some(&u.uid.to_string()), serde_json::json!({})).await?;
            println!("unbanned {}", u.email);
        }
        Cmd::User(UserCmd::VerifyEmail { ident }) => {
            let u = find(&pool, &ident).await?;
            sqlx::query("UPDATE users SET email_verified_at = COALESCE(email_verified_at, now()) WHERE uid = $1")
                .bind(u.uid)
                .execute(&pool)
                .await?;
            store::audit(&pool, &actor, "user.verify_email", Some(&u.uid.to_string()), serde_json::json!({})).await?;
            println!("marked {} verified", u.email);
        }
        Cmd::User(UserCmd::RotatePlayKey { ident }) => {
            let u = find(&pool, &ident).await?;
            store::rotate_play_key(&pool, u.uid).await?;
            store::audit(&pool, &actor, "user.rotate_play_key", Some(&u.uid.to_string()), serde_json::json!({}))
                .await?;
            println!("play key rotated for {}", u.email);
        }
        Cmd::User(UserCmd::SetCode { ident, code }) => {
            let u = find(&pool, &ident).await?;
            let code = common::codes::parse_connect_code(&code).map_err(|e| anyhow::anyhow!("{e}"))?.to_string();
            store::admin_set_connect_code(&pool, u.uid, &code).await?;
            store::audit(
                &pool,
                &actor,
                "user.set_code",
                Some(&u.uid.to_string()),
                serde_json::json!({"from": u.connect_code, "to": code}),
            )
            .await?;
            println!("{} now has connect code {code}", u.email);
        }
    }
    Ok(())
}
