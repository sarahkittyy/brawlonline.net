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

/// What `ruleset delete` / `ruleset rename` changed: the versions (code, version, name before),
/// and the ruleset's other versions it left alone.
#[derive(Debug)]
pub struct RulesetChange {
    pub changed: Vec<(String, i32, String)>,
    pub others: Vec<(String, i32)>,
}

/// The published versions `code` names: itself, or with `all_versions` every version of its
/// ruleset; and the ruleset's other versions. Locked for the transaction.
async fn ruleset_versions(
    tx: &mut sqlx::Transaction<'_, sqlx::Postgres>,
    code: &str,
    all_versions: bool,
) -> anyhow::Result<RulesetChange> {
    let code = common::rules::parse_ruleset_code(code).ok_or_else(|| anyhow::anyhow!("{code:?} is no ruleset code"))?;
    let lineage: uuid::Uuid = sqlx::query_scalar("SELECT lineage FROM rulesets WHERE code = $1")
        .bind(&code)
        .fetch_optional(&mut **tx)
        .await?
        .ok_or_else(|| anyhow::anyhow!("no ruleset {code}"))?;
    let rows: Vec<(String, i32, String)> =
        sqlx::query_as("SELECT code, version, name FROM rulesets WHERE lineage = $1 ORDER BY version FOR UPDATE")
            .bind(lineage)
            .fetch_all(&mut **tx)
            .await?;
    let (changed, others): (Vec<_>, Vec<_>) = rows.into_iter().partition(|r| all_versions || r.0 == code);
    Ok(RulesetChange { changed, others: others.into_iter().map(|(c, v, _)| (c, v)).collect() })
}

/// Lets this transaction change published rulesets (0014_ruleset_plays.sql).
async fn ruleset_admin(tx: &mut sqlx::Transaction<'_, sqlx::Postgres>) -> sqlx::Result<()> {
    sqlx::query("SET LOCAL pp.ruleset_admin = 'on'").execute(&mut **tx).await.map(|_| ())
}

async fn audit_tx(
    tx: &mut sqlx::Transaction<'_, sqlx::Postgres>,
    actor: &str,
    action: &str,
    target: &str,
    detail: serde_json::Value,
) -> sqlx::Result<()> {
    sqlx::query("INSERT INTO audit_log (actor, action, target, detail) VALUES ($1, $2, $3, $4)")
        .bind(actor)
        .bind(action)
        .bind(target)
        .bind(sqlx::types::Json(detail))
        .execute(&mut **tx)
        .await
        .map(|_| ())
}

/// Deletes a published ruleset version (with `all_versions`, the whole ruleset) for good: its
/// game count, and every account's remembered or recent use of it; drafts made from it keep their
/// rules but no longer edit it. Match history keeps the name it was played under (its link finds
/// nothing). A room playing it in mm plays it on until its rules change.
pub async fn delete_ruleset(
    pool: &PgPool,
    actor: &str,
    code: &str,
    all_versions: bool,
) -> anyhow::Result<RulesetChange> {
    let mut tx = pool.begin().await?;
    ruleset_admin(&mut tx).await?;
    let change = ruleset_versions(&mut tx, code, all_versions).await?;
    let codes: Vec<&str> = change.changed.iter().map(|r| r.0.as_str()).collect();
    sqlx::query("DELETE FROM remembered_rulesets WHERE code = ANY($1)").bind(&codes).execute(&mut *tx).await?;
    sqlx::query("DELETE FROM ruleset_recent WHERE code = ANY($1)").bind(&codes).execute(&mut *tx).await?;
    sqlx::query("UPDATE ruleset_drafts SET based_on = NULL WHERE based_on = ANY($1)")
        .bind(&codes)
        .execute(&mut *tx)
        .await?;
    sqlx::query("DELETE FROM rulesets WHERE code = ANY($1)").bind(&codes).execute(&mut *tx).await?;
    audit_tx(
        &mut tx,
        actor,
        "ruleset.delete",
        &codes.join(","),
        serde_json::json!({"versions": change.changed.iter().map(|(c, v, n)| serde_json::json!({"code": c, "version": v, "name": n})).collect::<Vec<_>>()}),
    )
    .await?;
    tx.commit().await?;
    Ok(change)
}

/// Renames a published ruleset version (with `all_versions`, every version of the ruleset): the
/// name in its row and its JSON, and so its hash (`Ruleset::hash` covers the name). The name must
/// pass the publisher's own rule (`Ruleset::validate`). Match history keeps the name and hash it
/// was played under.
pub async fn rename_ruleset(
    pool: &PgPool,
    actor: &str,
    code: &str,
    name: &str,
    all_versions: bool,
) -> anyhow::Result<RulesetChange> {
    let mut tx = pool.begin().await?;
    ruleset_admin(&mut tx).await?;
    let change = ruleset_versions(&mut tx, code, all_versions).await?;
    for (code, _, _) in &change.changed {
        let json: String =
            sqlx::query_scalar("SELECT json FROM rulesets WHERE code = $1").bind(code).fetch_one(&mut *tx).await?;
        let mut r: common::rules::Ruleset = serde_json::from_str(&json)?;
        r.name = name.to_string();
        r.validate().map_err(|e| anyhow::anyhow!("{name:?}: {e}"))?;
        sqlx::query("UPDATE rulesets SET name = $2, json = $3, hash = $4 WHERE code = $1")
            .bind(code)
            .bind(name)
            .bind(r.canonical_json())
            .bind(r.hash())
            .execute(&mut *tx)
            .await?;
    }
    audit_tx(
        &mut tx,
        actor,
        "ruleset.rename",
        &change.changed.iter().map(|r| r.0.as_str()).collect::<Vec<_>>().join(","),
        serde_json::json!({"to": name, "versions": change.changed.iter().map(|(c, v, n)| serde_json::json!({"code": c, "version": v, "from": n})).collect::<Vec<_>>()}),
    )
    .await?;
    tx.commit().await?;
    Ok(change)
}

// ---- Speed (`common::speed`, docs/design/speed-check.md): log only, nothing is refused ----

/// One thing a player's PC did, as `speed_reports` and `speed_checks` have it.
#[derive(Debug, Clone, sqlx::FromRow)]
pub struct SpeedEventRow {
    pub at: chrono::DateTime<chrono::Utc>,
    /// `game` (a speed report) or `check` (the waiting room).
    pub kind: String,
    pub mode: String,
    pub match_id: Option<String>,
    pub frames: Option<i32>,
    pub fps: Option<f32>,
    pub own_late: Option<f32>,
    pub followed: Option<f32>,
    pub peer_fps: Option<f32>,
    pub windows: Option<Vec<f32>>,
    pub backend: Option<String>,
    pub platform: String,
    pub app_version: String,
    // The game's input delay and what it cost on this screen (`common::speed::GameDelay`; games
    // of builds before 2026-10-22 have none).
    pub delay: Option<i16>,
    pub delay_profile: Option<String>,
    pub delay_asked: Option<String>,
    pub delay_fixed: Option<bool>,
    pub rtt_start: Option<f32>,
    pub ping: Option<f32>,
    pub jitter: Option<f32>,
    pub rollbacks: Option<f32>,
    pub depth: Option<f32>,
    pub rollback_ms: Option<f32>,
    pub late_after_rollback: Option<f32>,
    pub hitches: Option<f32>,
    pub waits: Option<i32>,
    pub wait_ms: Option<f32>,
    pub resim_ms: Option<f32>,
}

impl SpeedEventRow {
    /// A game's speed as its game sent it.
    pub fn perf(&self) -> Option<common::speed::GamePerf> {
        (self.kind == "game").then(|| common::speed::GamePerf {
            frames: self.frames.unwrap_or(0).max(0) as u32,
            fps: self.fps.unwrap_or(0.0) as f64,
            own_late: self.own_late.unwrap_or(0.0) as f64,
            followed: self.followed.unwrap_or(0.0) as f64,
            peer_fps: self.peer_fps.map(f64::from),
            slow: false,
            peer_slow: false,
            delay: self.game_delay(),
        })
    }

    fn game_delay(&self) -> Option<Box<common::speed::GameDelay>> {
        let f = |v: Option<f32>| v.unwrap_or(0.0) as f64;
        Some(Box::new(common::speed::GameDelay {
            delay: self.delay?.max(0) as u32,
            delay_profile: self.delay_profile.clone()?,
            delay_asked: self.delay_asked.clone().unwrap_or_default(),
            delay_fixed: self.delay_fixed.unwrap_or(false),
            rtt_start: self.rtt_start.map_or(-1.0, f64::from),
            ping: f(self.ping),
            jitter: f(self.jitter),
            rollbacks: f(self.rollbacks),
            depth: f(self.depth),
            rollback_ms: f(self.rollback_ms),
            late_after_rollback: f(self.late_after_rollback),
            hitches: f(self.hitches),
            waits: self.waits.unwrap_or(0).max(0) as u32,
            wait_ms: f(self.wait_ms),
            resim_ms: f(self.resim_ms),
        }))
    }

    pub fn check(&self) -> Option<common::speed::Check> {
        let w: Vec<f64> = self.windows.as_ref()?.iter().map(|w| *w as f64).collect();
        Some(common::speed::check_verdict(&w))
    }

    /// What the lock rule reads: Ranked games and every check.
    pub fn event(&self) -> Option<common::speed::Event> {
        match self.kind.as_str() {
            "game" if self.mode == "ranked" => {
                Some(common::speed::Event::RankedGame { slow: self.perf()?.slow_game() })
            }
            "check" => Some(common::speed::Event::Check(self.check()?)),
            _ => None,
        }
    }
}

/// A player's games and checks of the last `days` days, oldest first.
pub async fn speed_events(pool: &PgPool, uid: uuid::Uuid, days: i64) -> anyhow::Result<Vec<SpeedEventRow>> {
    Ok(sqlx::query_as::<_, SpeedEventRow>(
        "SELECT created_at AS at, 'game' AS kind, mode, match_id, frames, fps, own_late, followed, peer_fps,
                NULL::real[] AS windows, NULL::text AS backend, platform, app_version,
                delay, delay_profile, delay_asked, delay_fixed, rtt_start, ping, jitter, rollbacks, depth,
                rollback_ms, late_after_rollback, hitches, waits, wait_ms, resim_ms
           FROM speed_reports WHERE uid = $1 AND created_at > now() - make_interval(days => $2::int)
         UNION ALL
         SELECT created_at, 'check', mode, NULL, NULL, NULL, NULL, NULL, NULL, windows, backend, platform, app_version,
                NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
           FROM speed_checks WHERE uid = $1 AND created_at > now() - make_interval(days => $2::int)
         ORDER BY at",
    )
    .bind(uid)
    .bind(days as i32)
    .fetch_all(pool)
    .await?)
}

/// A player the rule would lock out of Ranked now.
#[derive(Debug, Clone)]
pub struct SpeedLocked {
    pub uid: uuid::Uuid,
    pub connect_code: Option<String>,
    pub display_name: Option<String>,
    /// Slow Ranked games in a row with no passing check since.
    pub streak: u32,
    /// The last Ranked game.
    pub last: SpeedEventRow,
}

/// Everyone with a slow Ranked game in the last `days` days whom the rule would lock now, by
/// their history over those days, latest first.
pub async fn speed_locked(pool: &PgPool, days: i64) -> anyhow::Result<Vec<SpeedLocked>> {
    use common::speed::{MIN_GAME_FRAMES, SLOW_FPS, SLOW_OWN_LATE};
    let candidates: Vec<(uuid::Uuid, Option<String>, Option<String>)> = sqlx::query_as(
        "SELECT DISTINCT r.uid, u.connect_code, u.display_name
           FROM speed_reports r LEFT JOIN users u ON u.uid = r.uid
          WHERE r.mode = 'ranked' AND r.frames >= $1 AND r.fps < $2 AND r.own_late > $3
            AND r.created_at > now() - make_interval(days => $4::int)",
    )
    .bind(MIN_GAME_FRAMES as i32)
    .bind(SLOW_FPS as f32)
    .bind(SLOW_OWN_LATE as f32)
    .bind(days as i32)
    .fetch_all(pool)
    .await?;
    let mut out = Vec::new();
    for (uid, connect_code, display_name) in candidates {
        let rows = speed_events(pool, uid, days).await?;
        let events: Vec<_> = rows.iter().filter_map(SpeedEventRow::event).collect();
        if !common::speed::locked(&events) {
            continue;
        }
        let streak = common::speed::slow_streak(&events);
        if let Some(last) = rows.into_iter().rev().find(|r| r.kind == "game" && r.mode == "ranked") {
            out.push(SpeedLocked { uid, connect_code, display_name, streak, last });
        }
    }
    out.sort_by(|a, b| b.last.at.cmp(&a.last.at));
    Ok(out)
}

/// Waiting-room checks of one platform and video backend.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SpeedCheckCounts {
    pub platform: String,
    pub backend: String,
    pub pass: i64,
    pub slow: i64,
    pub unsure: i64,
    pub players: i64,
}

/// The numbers over the last `days` days.
#[derive(Debug, Clone, Default)]
pub struct SpeedSummary {
    /// Per mode: games reported, slow ones (this PC's own), players with a slow one.
    pub games: Vec<(String, i64, i64, i64)>,
    pub checks: Vec<SpeedCheckCounts>,
    /// Players the Ranked rule would lock now.
    pub locked: usize,
}

pub async fn speed_summary(pool: &PgPool, days: i64) -> anyhow::Result<SpeedSummary> {
    use common::speed::{Check, MIN_GAME_FRAMES, SLOW_FPS, SLOW_OWN_LATE};
    let games: Vec<(String, i64, i64, i64)> = sqlx::query_as(
        "SELECT mode, count(*),
                count(*) FILTER (WHERE frames >= $1 AND fps < $2 AND own_late > $3),
                count(DISTINCT uid) FILTER (WHERE frames >= $1 AND fps < $2 AND own_late > $3)
           FROM speed_reports WHERE created_at > now() - make_interval(days => $4::int)
          GROUP BY mode ORDER BY count(*) DESC",
    )
    .bind(MIN_GAME_FRAMES as i32)
    .bind(SLOW_FPS as f32)
    .bind(SLOW_OWN_LATE as f32)
    .bind(days as i32)
    .fetch_all(pool)
    .await?;
    let rows: Vec<(uuid::Uuid, String, String, Vec<f32>)> = sqlx::query_as(
        "SELECT uid, platform, backend, windows FROM speed_checks
          WHERE created_at > now() - make_interval(days => $1::int)",
    )
    .bind(days as i32)
    .fetch_all(pool)
    .await?;
    type Counts = (SpeedCheckCounts, std::collections::HashSet<uuid::Uuid>);
    let mut by: std::collections::BTreeMap<(String, String), Counts> = Default::default();
    for (uid, platform, backend, windows) in rows {
        let w: Vec<f64> = windows.iter().map(|w| *w as f64).collect();
        let (c, who) = by.entry((platform.clone(), backend.clone())).or_insert_with(|| {
            (SpeedCheckCounts { platform, backend, ..Default::default() }, Default::default())
        });
        match common::speed::check_verdict(&w) {
            Check::Pass => c.pass += 1,
            Check::Slow => c.slow += 1,
            Check::Unsure => c.unsure += 1,
        }
        who.insert(uid);
    }
    let checks = by.into_values().map(|(c, who)| SpeedCheckCounts { players: who.len() as i64, ..c }).collect();
    Ok(SpeedSummary { games, checks, locked: speed_locked(pool, days).await?.len() })
}
