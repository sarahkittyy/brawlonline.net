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

/// `admin ranked recompute`: every rank and placement from the rating history, audit-logged.
pub async fn recompute_ranked_tiers(pool: &PgPool, actor: &str) -> anyhow::Result<accounts::ranked::Recomputed> {
    let done = accounts::ranked::recompute_tiers(pool).await?;
    store::audit(
        pool,
        actor,
        "ranked.recompute_tiers",
        None,
        serde_json::json!({ "players": done.players, "sets": done.sets, "ranks_changed": done.ranks_changed }),
    )
    .await?;
    Ok(done)
}

/// What `admin ranked void` did (or, with `dry_run`, would do).
#[derive(Debug, Clone)]
pub struct Voided {
    /// The set's status before (`COMPLETE` or `ABANDONED`).
    pub previous_status: String,
    /// The set's own rating changes, deleted.
    pub deleted: usize,
    /// Rating changes settled since the set (the replayed window, the set's own included).
    pub window: usize,
    /// Later rating changes whose values changed.
    pub rerated: usize,
    /// Players whose rating changed: (uid, connect code, before, after), biggest change first.
    pub moved: Vec<(uuid::Uuid, Option<String>, f64, f64)>,
    pub ranks: accounts::ranked::Recomputed,
}

/// `admin ranked void`: voids a rated ranked set as if it had never been played, in one
/// transaction with `ratings` locked against settles.
///
/// Elo depends on the order of sets, so every rating change settled since the set is replayed
/// without it ([`common::ranked::rerate`]): the set's players' later sets, their opponents' later
/// sets, and so on. First the window is replayed unchanged, and the void is refused unless that
/// reproduces every stored row and every player's `ratings` row exactly (a history corrected by
/// hand, or rated under rules `rerate` does not know, needs a look first). Then the later rows and
/// the moved players' ratings are rewritten, the set's own rows deleted (its players get back the
/// rated set, and the win or loss), the set marked `TERMINATED` with `voided by admin: <reason>`,
/// ranks and RDs recomputed, and an audit row (`ranked.void_set`) keeps every old value.
/// `dry_run` does all of it and rolls back.
pub async fn void_ranked_set(
    pool: &PgPool,
    actor: &str,
    match_id: &str,
    reason: &str,
    dry_run: bool,
) -> anyhow::Result<Voided> {
    use common::ranked::{rerate, StoredChange};
    use std::collections::HashMap;
    use uuid::Uuid;

    anyhow::ensure!(!reason.trim().is_empty(), "a reason is required");
    let mut tx = pool.begin().await?;
    // EXCLUSIVE, not recompute's SHARE ROW EXCLUSIVE: a settle's `SELECT ... FOR UPDATE` on two
    // rows waits here too, so it cannot hold a row this rewrites.
    sqlx::query("LOCK TABLE ratings IN EXCLUSIVE MODE").execute(&mut *tx).await?;
    let row: Option<(i16, String, Option<Uuid>, Option<String>)> =
        sqlx::query_as("SELECT mode, status, winner, end_reason FROM mm_matches WHERE match_id = $1 FOR UPDATE")
            .bind(match_id)
            .fetch_optional(&mut *tx)
            .await?;
    let Some((mode, status, winner, end_reason)) = row else { anyhow::bail!("no match {match_id:?}") };
    anyhow::ensure!(mode == accounts::ranked::MODE_RANKED, "{match_id} is not a ranked set");
    anyhow::ensure!(
        status == "COMPLETE" || status == "ABANDONED",
        "{match_id} is {status}: only a COMPLETE or ABANDONED set is rated"
    );

    #[allow(clippy::type_complexity)]
    let rows: Vec<(i64, Uuid, String, Option<Uuid>, f64, f64, f64, f64, f64, i32, chrono::DateTime<chrono::Utc>)> =
        sqlx::query_as(
            "SELECT id, uid, match_id, opponent, score, rating_before, rating_after, opponent_rating, k,
                    sets_before, created_at
               FROM rating_events
              WHERE created_at >= (SELECT min(created_at) FROM rating_events WHERE match_id = $1)
              ORDER BY created_at, id",
        )
        .bind(match_id)
        .fetch_all(&mut *tx)
        .await?;
    let window: Vec<StoredChange> = rows
        .into_iter()
        .map(|(id, uid, match_id, opponent, score, rating_before, rating_after, opponent_rating, k, n, at)| {
            StoredChange {
                id,
                uid,
                match_id,
                opponent,
                score,
                rating_before,
                rating_after,
                opponent_rating,
                k,
                sets_before: n.max(0) as u32,
                at,
            }
        })
        .collect();
    let own: Vec<&StoredChange> = window.iter().filter(|s| s.match_id == match_id).collect();
    anyhow::ensure!(!own.is_empty(), "{match_id} changed nobody's rating");

    // The stored history must be what replaying it gives, and end where `ratings` stands.
    let (same, _) = rerate(&window, &[]);
    if let Some((r, s)) = same.iter().zip(&window).find(|(r, s)| !r.matches(s)) {
        anyhow::bail!(
            "replaying the rating history does not reproduce rating_events row {} ({} in {}): stored \
             {} -> {} against {} (k {}, {} sets before), replayed {} -> {} against {} (k {}, {} sets before); \
             nothing was changed",
            s.id,
            s.uid,
            s.match_id,
            s.rating_before,
            s.rating_after,
            s.opponent_rating,
            s.k,
            s.sets_before,
            r.rating_before,
            r.rating_after,
            r.opponent_rating,
            r.k,
            r.sets_before
        );
    }
    let mut last: HashMap<Uuid, &StoredChange> = HashMap::new();
    for s in &window {
        last.insert(s.uid, s);
    }
    let uids: Vec<Uuid> = last.keys().copied().collect();
    let stored: Vec<(Uuid, f64, i32)> =
        sqlx::query_as("SELECT uid, rating, sets_played FROM ratings WHERE uid = ANY($1)")
            .bind(&uids)
            .fetch_all(&mut *tx)
            .await?;
    for (uid, rating, n) in &stored {
        let s = last[uid];
        anyhow::ensure!(
            *rating == s.rating_after && *n == s.sets_before as i32 + 1,
            "ratings row of {uid} ({rating}, {n} sets) is not where its last rating change left it \
             ({}, {} sets); nothing was changed",
            s.rating_after,
            s.sets_before + 1
        );
    }

    let removed: Vec<i64> = own.iter().map(|s| s.id).collect();
    let (rerated, finals) = rerate(&window, &removed);
    let by_id: HashMap<i64, &StoredChange> = window.iter().map(|s| (s.id, s)).collect();
    let changed: Vec<_> = rerated.iter().filter(|r| !r.matches(by_id[&r.id])).collect();
    sqlx::query(
        "UPDATE rating_events e
            SET rating_before = v.rb, rating_after = v.ra, opponent_rating = v.o, k = v.k, sets_before = v.n
           FROM unnest($1::bigint[], $2::float8[], $3::float8[], $4::float8[], $5::float8[], $6::int[])
                AS v(id, rb, ra, o, k, n)
          WHERE e.id = v.id",
    )
    .bind(changed.iter().map(|r| r.id).collect::<Vec<_>>())
    .bind(changed.iter().map(|r| r.rating_before).collect::<Vec<_>>())
    .bind(changed.iter().map(|r| r.rating_after).collect::<Vec<_>>())
    .bind(changed.iter().map(|r| r.opponent_rating).collect::<Vec<_>>())
    .bind(changed.iter().map(|r| r.k).collect::<Vec<_>>())
    .bind(changed.iter().map(|r| r.sets_before as i32).collect::<Vec<_>>())
    .execute(&mut *tx)
    .await?;

    // Every player of the window ends where the replay leaves them; the set's players also get
    // back the rated set and its win or loss.
    let score: HashMap<Uuid, f64> = own.iter().map(|s| (s.uid, s.score)).collect();
    let mut moved = vec![];
    for uid in &uids {
        let (before, after) = (last[uid].rating_after, finals[uid]);
        let s = score.get(uid);
        sqlx::query(
            "UPDATE ratings SET rating = $2, sets_played = $3, wins = wins - $4, losses = losses - $5
              WHERE uid = $1",
        )
        .bind(uid)
        .bind(after.rating)
        .bind(after.sets_played as i32)
        .bind(i32::from(s.is_some_and(|&s| s > 0.5)))
        .bind(i32::from(s.is_some_and(|&s| s < 0.5)))
        .execute(&mut *tx)
        .await?;
        if after.rating != before {
            moved.push((*uid, before, after.rating));
        }
    }
    let deleted = sqlx::query("DELETE FROM rating_events WHERE match_id = $1")
        .bind(match_id)
        .execute(&mut *tx)
        .await?
        .rows_affected() as usize;
    sqlx::query("UPDATE mm_matches SET status = 'TERMINATED', winner = NULL, end_reason = $2 WHERE match_id = $1")
        .bind(match_id)
        .bind(format!("voided by admin: {reason}"))
        .execute(&mut *tx)
        .await?;
    let ranks = accounts::ranked::recompute_tiers_in(&mut tx).await?;

    let values = |s: &StoredChange| {
        serde_json::json!({
            "rating_before": s.rating_before, "rating_after": s.rating_after,
            "opponent_rating": s.opponent_rating, "sets_before": s.sets_before,
        })
    };
    let detail = serde_json::json!({
        "reason": reason,
        "previous_status": status,
        "previous_winner": winner,
        "previous_end_reason": end_reason,
        "events": own.iter().map(|s| serde_json::json!({
            "id": s.id, "uid": s.uid, "score": s.score, "rating_before": s.rating_before,
            "rating_after": s.rating_after, "sets_before": s.sets_before,
        })).collect::<Vec<_>>(),
        "rerated": changed.iter().map(|r| serde_json::json!({
            "id": r.id, "uid": by_id[&r.id].uid, "match_id": by_id[&r.id].match_id,
            "before": values(by_id[&r.id]),
            "after": {
                "rating_before": r.rating_before, "rating_after": r.rating_after,
                "opponent_rating": r.opponent_rating, "sets_before": r.sets_before,
            },
        })).collect::<Vec<_>>(),
        "ratings": moved.iter().map(|(u, b, a)| (u.to_string(), serde_json::json!([b, a])))
            .collect::<serde_json::Map<_, _>>(),
    });
    sqlx::query("INSERT INTO audit_log (actor, action, target, detail) VALUES ($1, 'ranked.void_set', $2, $3)")
        .bind(actor)
        .bind(match_id)
        .bind(&detail)
        .execute(&mut *tx)
        .await?;

    let codes: HashMap<Uuid, Option<String>> =
        sqlx::query_as::<_, (Uuid, Option<String>)>("SELECT uid, connect_code FROM users WHERE uid = ANY($1)")
            .bind(&uids)
            .fetch_all(&mut *tx)
            .await?
            .into_iter()
            .collect();
    if dry_run {
        tx.rollback().await?;
    } else {
        tx.commit().await?;
    }
    moved.sort_by(|a, b| (b.2 - b.1).abs().total_cmp(&(a.2 - a.1).abs()));
    Ok(Voided {
        previous_status: status,
        deleted,
        window: window.len(),
        rerated: changed.len(),
        moved: moved.into_iter().map(|(u, b, a)| (u, codes.get(&u).cloned().flatten(), b, a)).collect(),
        ranks,
    })
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
