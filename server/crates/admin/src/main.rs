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
#[command(name = "admin", version, about = "Admin CLI: users, password resets, bans")]
struct Cli {
    #[arg(long, env = "DATABASE_URL", global = true, hide_env_values = true)]
    database_url: Option<String>,
    /// Where accounts keeps bug reports' logs zips (as accounts reads it; relative to the
    /// working directory, which pp-admin sets to /var/lib/ppserver).
    #[arg(long, env = "BUG_REPORTS_DIR", global = true, default_value = "bug-reports")]
    bug_reports_dir: std::path::PathBuf,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Apply database migrations.
    Migrate,
    /// Print a new random PLAY_KEY_SECRET.
    GenSecret,
    /// Accounts. IDENT is a uid, an email or a connect code.
    #[command(subcommand)]
    User(UserCmd),
    /// Chat reports nobody has handled yet, newest first: who reported whom in which group, the
    /// reason, and each message (cleaned) with whether its signature checked out.
    ChatReports {
        #[arg(long, default_value_t = 50)]
        limit: i64,
    },
    /// Mark a chat report handled (it leaves `chat-reports`).
    ChatReportDone { id: i64 },
    /// Shadowban an account from chat (IDENT as for `user`): its room, match and spectator chat
    /// messages and launcher DMs reach nobody, while its own game and launcher show them as sent
    /// and it still sees everyone else's. Takes effect within seconds, also mid-conversation.
    Mute {
        ident: String,
        /// Why (kept in audit_log).
        #[arg(long)]
        reason: Option<String>,
    },
    /// Lift a `mute`.
    Unmute { ident: String },
    /// Bug reports from the launcher's bug button, with their logs zips.
    #[command(subcommand)]
    Bug(BugCmd),
    /// Ranked: ranks and placement (docs/design/ranked-tiers.md).
    #[command(subcommand)]
    Ranked(RankedCmd),
    /// Published rulesets (docs/rulesets-protocol.md). CODE is a ruleset code (one version).
    #[command(subcommand)]
    Ruleset(RulesetCmd),
    /// How fast players' PCs run the game (docs/design/speed-check.md). Log only: nothing is
    /// refused yet; this shows what the Ranked rule would do.
    #[command(subcommand)]
    Speed(SpeedCmd),
}

#[derive(Subcommand)]
enum SpeedCmd {
    /// Games reported per mode and how many were slow; waiting-room checks per platform and video
    /// backend; how many players the Ranked rule would lock now.
    Summary {
        #[arg(long, default_value_t = 7)]
        days: i64,
    },
    /// Players the rule would lock out of Ranked now (two slow Ranked games in a row, no passing
    /// waiting-room check since), latest first.
    Locked {
        #[arg(long, default_value_t = 30)]
        days: i64,
    },
    /// One player's games and checks, oldest first, and what the rule says. IDENT as for `user`.
    Show {
        ident: String,
        #[arg(long, default_value_t = 30)]
        days: i64,
    },
}

#[derive(Subcommand)]
enum RulesetCmd {
    /// Delete a published version for good (its /r/ link stops working): its game count, and
    /// every account's remembered or recent use of it; drafts made from it keep their rules.
    Delete {
        code: String,
        /// Every version of the ruleset, not only CODE.
        #[arg(long)]
        all_versions: bool,
        /// Required: confirms the deletion.
        #[arg(long)]
        yes: bool,
    },
    /// Rename a published version (its JSON and hash too). The name must pass the publisher's
    /// own rule. Match history keeps the name it was played under.
    Rename {
        code: String,
        name: String,
        /// Every version of the ruleset, not only CODE.
        #[arg(long)]
        all_versions: bool,
    },
}

#[derive(Subcommand)]
enum RankedCmd {
    /// Recompute every player's rank and placement from the rating history. Run it after changing
    /// the rank cutoffs, or after a manual correction of ratings or rating_events.
    Recompute,
    /// Void a rated set (COMPLETE or ABANDONED) as if it was never played: its rating changes are
    /// taken back and every set settled since is re-rated without it, ranks recomputed, all in one
    /// transaction and audit-logged. Refused if the stored history does not replay exactly.
    Void {
        /// The set's match id (mm_matches.match_id).
        match_id: String,
        /// Why (stored in the set's end_reason and the audit row).
        #[arg(long)]
        reason: String,
        /// Do everything, print what changes, and roll it back.
        #[arg(long)]
        dry_run: bool,
    },
}

#[derive(Subcommand)]
enum BugCmd {
    /// Unresolved reports, newest first: id, when, who, launcher version, zip size, first line.
    List {
        /// Resolved ones too.
        #[arg(long)]
        all: bool,
        #[arg(long, default_value_t = 50)]
        limit: i64,
    },
    /// One report in full, with the path of its logs zip.
    Show { id: i64 },
    /// Mark a report resolved (it leaves `bug list`). The zip stays until `bug delete` / `bug clear`.
    Resolve {
        id: i64,
        /// What fixed it (kept with the report).
        #[arg(long, default_value = "")]
        note: String,
    },
    /// Put a resolved report back on the list.
    Reopen { id: i64 },
    /// Delete reports and their zips for good.
    Delete {
        #[arg(required = true)]
        ids: Vec<i64>,
    },
    /// Delete every resolved report and its zip for good.
    Clear {
        /// Required: confirms the deletion.
        #[arg(long)]
        yes: bool,
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
    /// Ban an account (permanent unless --days). Ends its sessions and rotates its play key, and
    /// within seconds takes it out of its room (forfeiting a set that is on), queue, chat and
    /// running game; its launcher logs out and its game stops reconnecting.
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
    /// Change the display name, with the launcher's rule (at most 15 characters the game's font
    /// draws). Rooms, chat, the online list and the player's own launcher show it within seconds
    /// (their running game shows it after a restart); the player can still rename themselves.
    Rename {
        ident: String,
        name: String,
    },
    /// Change the connect code (the one admin-approved change).
    SetCode {
        ident: String,
        code: String,
    },
    /// Delete an account with its sessions and email tokens. Its connect code is free again
    /// at once. Cannot be undone.
    Delete {
        ident: String,
        /// Required: confirms the deletion.
        #[arg(long)]
        yes: bool,
    },
}

fn actor() -> String {
    let who = std::env::var("USER").or_else(|_| std::env::var("USERNAME")).unwrap_or_else(|_| "unknown".into());
    format!("admin-cli:{who}")
}

async fn find(pool: &PgPool, ident: &str) -> anyhow::Result<UserRow> {
    admin::find(pool, ident).await
}

fn who(uid: Option<uuid::Uuid>, code: &Option<String>, name: &Option<String>) -> String {
    match uid {
        Some(uid) => format!("{} ({}) {uid}", code.as_deref().unwrap_or("-"), name.as_deref().unwrap_or("")),
        None => "(deleted account)".into(),
    }
}

fn print_chat_report(r: &admin::ChatReportRow) {
    println!("#{}  {}  {}", r.id, r.created_at.format("%Y-%m-%d %H:%M UTC"), r.group_id);
    println!("  reporter: {}", who(r.reporter, &r.reporter_code, &r.reporter_name));
    println!("  reported: {}", who(r.reported, &r.reported_code, &r.reported_name));
    if !r.reason.is_empty() {
        println!("  reason:   {}", r.reason);
    }
    println!("  messages: {} of {} verified", r.verified, r.total);
    for m in r.messages.as_array().into_iter().flatten() {
        let mark = if m["verified"] == true { "verified  " } else { "NOT SIGNED" };
        println!("    [{mark}] #{:<4} {}", m["seq"], m["text"].as_str().unwrap_or(""));
    }
}

fn print_bug_report(r: &admin::BugReportRow, dir: &std::path::Path, full: bool) {
    let who = match r.uid {
        Some(_) => who(r.uid, &r.connect_code, &r.display_name),
        None => "(logged out or deleted account)".into(),
    };
    let logs = match r.logs_bytes {
        Some(n) if n < 1024 * 1024 => format!("{} KB", (n + 1023) / 1024),
        Some(n) => format!("{:.1} MB", n as f64 / (1024.0 * 1024.0)),
        None => "no logs".into(),
    };
    let state = match r.resolved_at {
        Some(t) => format!("  RESOLVED {}", t.format("%Y-%m-%d %H:%M UTC")),
        None => String::new(),
    };
    println!("#{}  {}  {}  [{}]{state}", r.id, r.created_at.format("%Y-%m-%d %H:%M UTC"), who, logs);
    if !full {
        let first = r.description.lines().find(|l| !l.trim().is_empty()).unwrap_or("(no description)");
        let mut first: String = first.chars().take(100).collect();
        if r.description.trim().chars().count() > first.chars().count() {
            first.push_str(" ...");
        }
        println!("  {first}");
        return;
    }
    println!("  launcher:   {}", r.launcher_version);
    println!("  platform:   {}", r.platform);
    if r.logs_bytes.is_some() {
        let path = accounts::bug_reports::logs_path(dir, r.id);
        let path = std::path::absolute(&path).unwrap_or(path);
        println!("  logs:       {}", path.display());
    }
    if r.resolved_at.is_some() && !r.resolution.is_empty() {
        println!("  resolution: {}", r.resolution);
    }
    println!();
    if r.description.trim().is_empty() {
        println!("  (no description)");
    }
    for line in r.description.lines() {
        println!("  {line}");
    }
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
    match u.chat_muted_at {
        Some(t) => println!("chat muted:     since {}", t.format("%Y-%m-%d %H:%M UTC")),
        None => println!("chat muted:     no"),
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
            if let Some(done) = accounts::ranked::backfill_tiers(&pool).await? {
                println!("ranked tiers filled: {} players from {} rating changes", done.players, done.sets);
            }
        }
        Cmd::Ranked(RankedCmd::Recompute) => {
            let done = admin::recompute_ranked_tiers(&pool, &actor).await?;
            println!(
                "ranked tiers recomputed: {} players from {} rating changes, {} rank(s) changed",
                done.players, done.sets, done.ranks_changed
            );
        }
        Cmd::Ranked(RankedCmd::Void { match_id, reason, dry_run }) => {
            let v = admin::void_ranked_set(&pool, &actor, &match_id, &reason, dry_run).await?;
            println!(
                "{}{match_id} ({}) voided: {} rating change(s) deleted, {} of {} later ones re-rated, \
                 {} player(s) moved, {} rank(s) changed",
                if dry_run { "DRY RUN, rolled back: " } else { "" },
                v.previous_status,
                v.deleted,
                v.rerated,
                v.window - v.deleted,
                v.moved.len(),
                v.ranks.ranks_changed
            );
            for (uid, code, before, after) in v.moved.iter().take(15) {
                println!(
                    "  {:<12} {uid}  {before:.2} -> {after:.2} ({:+.2})",
                    code.as_deref().unwrap_or("-"),
                    after - before
                );
            }
            if v.moved.len() > 15 {
                println!("  ... {} more", v.moved.len() - 15);
            }
        }
        Cmd::Ruleset(RulesetCmd::Delete { code, all_versions, yes }) => {
            anyhow::ensure!(yes, "this deletes ruleset {code} for good; add --yes to confirm");
            let c = admin::delete_ruleset(&pool, &actor, &code, all_versions).await?;
            for (code, version, name) in &c.changed {
                println!("deleted {code} ({name:?} v{version})");
            }
            print_other_versions(&c.others);
        }
        Cmd::Ruleset(RulesetCmd::Rename { code, name, all_versions }) => {
            let c = admin::rename_ruleset(&pool, &actor, &code, &name, all_versions).await?;
            for (code, version, from) in &c.changed {
                println!("{code} v{version} renamed from {from:?} to {name:?}");
            }
            print_other_versions(&c.others);
        }
        Cmd::ChatReports { limit } => {
            let reports = admin::unhandled_chat_reports(&pool, limit.clamp(1, 1000)).await?;
            if reports.is_empty() {
                println!("no unhandled chat reports");
            }
            for r in &reports {
                print_chat_report(r);
            }
        }
        Cmd::ChatReportDone { id } => {
            anyhow::ensure!(admin::chat_report_done(&pool, &actor, id).await?, "no unhandled chat report #{id}");
            println!("chat report #{id} handled");
        }
        Cmd::Mute { ident, reason } => {
            let u = find(&pool, &ident).await?;
            store::set_muted(&pool, u.uid, true).await?;
            store::audit(&pool, &actor, "user.mute", Some(&u.uid.to_string()), serde_json::json!({"reason": reason}))
                .await?;
            println!(
                "muted {} ({}){}",
                u.display_name,
                u.connect_code.as_deref().unwrap_or("-"),
                if u.chat_muted_at.is_some() { "; it already was" } else { "" }
            );
        }
        Cmd::Unmute { ident } => {
            let u = find(&pool, &ident).await?;
            store::set_muted(&pool, u.uid, false).await?;
            store::audit(&pool, &actor, "user.unmute", Some(&u.uid.to_string()), serde_json::json!({})).await?;
            println!(
                "unmuted {} ({}){}",
                u.display_name,
                u.connect_code.as_deref().unwrap_or("-"),
                if u.chat_muted_at.is_none() { "; it was not muted" } else { "" }
            );
        }
        Cmd::Speed(SpeedCmd::Summary { days }) => {
            let days = days.clamp(1, 365);
            let s = admin::speed_summary(&pool, days).await?;
            println!("games, last {days} days: mode, reported, slow (this PC's own), players with a slow one");
            for (mode, n, slow, players) in &s.games {
                println!("  {mode:<10} {n:>7} {slow:>7} {players:>7}");
            }
            println!("waiting-room checks: platform, backend, pass, slow, unsure, players");
            for c in &s.checks {
                println!(
                    "  {:<10} {:<10} {:>7} {:>7} {:>7} {:>7}",
                    c.platform, c.backend, c.pass, c.slow, c.unsure, c.players
                );
            }
            println!("the Ranked rule would lock {} player(s) now (log only; see `speed locked`)", s.locked);
        }
        Cmd::Speed(SpeedCmd::Locked { days }) => {
            let locked = admin::speed_locked(&pool, days.clamp(1, 365)).await?;
            if locked.is_empty() {
                println!("nobody");
            }
            for l in &locked {
                println!(
                    "{}  {} slow Ranked game(s) in a row, last {} at {:.1} fps ({:.0}% late), {} {}",
                    who(Some(l.uid), &l.connect_code, &l.display_name),
                    l.streak,
                    l.last.at.format("%Y-%m-%d %H:%M UTC"),
                    l.last.fps.unwrap_or(0.0),
                    100.0 * l.last.own_late.unwrap_or(0.0),
                    l.last.platform,
                    l.last.app_version
                );
            }
        }
        Cmd::Speed(SpeedCmd::Show { ident, days }) => {
            let u = find(&pool, &ident).await?;
            let rows = admin::speed_events(&pool, u.uid, days.clamp(1, 365)).await?;
            for r in &rows {
                let when = r.at.format("%Y-%m-%d %H:%M UTC");
                if let Some(p) = r.perf() {
                    println!(
                        "{when}  game  {:<9} {:>6.2} fps, {:>3.0}% late (own), {:>3.0}% at the peer's speed{}{}  {}",
                        r.mode,
                        p.fps,
                        100.0 * p.own_late,
                        100.0 * p.followed,
                        p.peer_fps.map(|f| format!(" ({f:.1} fps)")).unwrap_or_default(),
                        if p.slow_game() { "  SLOW" } else { "" },
                        r.match_id.as_deref().unwrap_or("")
                    );
                } else if let Some(c) = r.check() {
                    println!(
                        "{when}  check {:<9} {:?} {c:?}  {} {}",
                        r.mode,
                        r.windows.as_deref().unwrap_or(&[]),
                        r.backend.as_deref().unwrap_or(""),
                        r.platform
                    );
                }
            }
            let events: Vec<_> = rows.iter().filter_map(admin::SpeedEventRow::event).collect();
            println!(
                "{} event(s); {} slow Ranked game(s) in a row: the rule would {}lock them (log only)",
                rows.len(),
                common::speed::slow_streak(&events),
                if common::speed::locked(&events) { "" } else { "not " }
            );
        }
        Cmd::Bug(BugCmd::List { all, limit }) => {
            let reports = admin::bug_reports(&pool, all, limit.clamp(1, 1000)).await?;
            if reports.is_empty() {
                println!("{}", if all { "no bug reports" } else { "no unresolved bug reports" });
            }
            for r in &reports {
                print_bug_report(r, &cli.bug_reports_dir, false);
            }
        }
        Cmd::Bug(BugCmd::Show { id }) => {
            print_bug_report(&admin::bug_report(&pool, id).await?, &cli.bug_reports_dir, true);
        }
        Cmd::Bug(BugCmd::Resolve { id, note }) => {
            anyhow::ensure!(
                admin::resolve_bug_report(&pool, &actor, id, &note).await?,
                "no unresolved bug report #{id}"
            );
            println!("bug report #{id} resolved");
        }
        Cmd::Bug(BugCmd::Reopen { id }) => {
            anyhow::ensure!(admin::reopen_bug_report(&pool, &actor, id).await?, "no resolved bug report #{id}");
            println!("bug report #{id} reopened");
        }
        Cmd::Bug(BugCmd::Delete { ids }) => {
            let deleted = admin::delete_bug_reports(&pool, &actor, &cli.bug_reports_dir, &ids).await?;
            let missing: Vec<_> = ids.iter().filter(|id| !deleted.contains(id)).collect();
            println!("deleted {} bug report(s) {deleted:?}", deleted.len());
            anyhow::ensure!(missing.is_empty(), "no bug report(s) {missing:?}");
        }
        Cmd::Bug(BugCmd::Clear { yes }) => {
            anyhow::ensure!(yes, "this deletes every resolved bug report and its logs for good; add --yes to confirm");
            let deleted = admin::delete_bug_reports(&pool, &actor, &cli.bug_reports_dir, &[]).await?;
            println!("deleted {} resolved bug report(s)", deleted.len());
        }
        Cmd::User(UserCmd::List) => {
            println!("{:<36} {:<10} {:<16} {:<30} {:<5} {:<6} muted", "uid", "code", "name", "email", "ver.", "banned");
            for u in store::list_users(&pool).await? {
                println!(
                    "{:<36} {:<10} {:<16} {:<30} {:<5} {:<6} {}",
                    u.uid,
                    u.connect_code.as_deref().unwrap_or("-"),
                    u.display_name,
                    u.email,
                    if u.email_verified() { "yes" } else { "no" },
                    if u.is_banned() { "yes" } else { "" },
                    if u.chat_muted_at.is_some() { "yes" } else { "" }
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
        Cmd::User(UserCmd::Rename { ident, name }) => {
            let u = find(&pool, &ident).await?;
            common::codes::validate_display_name(&name).map_err(|e| anyhow::anyhow!("{name:?}: {e}"))?;
            store::rename(&pool, u.uid, &name).await?;
            store::audit(
                &pool,
                &actor,
                "user.rename",
                Some(&u.uid.to_string()),
                serde_json::json!({"from": u.display_name, "to": name}),
            )
            .await?;
            println!("{} renamed from {:?} to {name:?}", u.email, u.display_name);
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
        Cmd::User(UserCmd::Delete { ident, yes }) => {
            let u = find(&pool, &ident).await?;
            anyhow::ensure!(yes, "this deletes {} ({}) for good; add --yes to confirm", u.email, u.uid);
            anyhow::ensure!(store::delete_user(&pool, u.uid).await?, "{} was already gone", u.uid);
            store::audit(
                &pool,
                &actor,
                "user.delete",
                Some(&u.uid.to_string()),
                serde_json::json!({"email": u.email, "connect_code": u.connect_code}),
            )
            .await?;
            println!("deleted {} ({})", u.email, u.uid);
        }
    }
    Ok(())
}

fn print_other_versions(others: &[(String, i32)]) {
    if !others.is_empty() {
        let list: Vec<String> = others.iter().map(|(c, v)| format!("{c} (v{v})")).collect();
        println!("left alone: {} (--all-versions includes them)", list.join(", "));
    }
}
