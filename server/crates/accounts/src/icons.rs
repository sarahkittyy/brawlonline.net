//! Badges and custom main icons (docs/design/profiles.md §10, `migrations/0037_badges.sql`): the
//! icon files (`GET /v1/icons/<id>.png`) and the database calls `pp-admin icon|badge|main-icon`
//! makes. Nothing here is reachable from the launcher but the files: only pp-admin gives badges.
//!
//! Every change to a player's badges or main icon also touches `users.updated_at`, so accounts'
//! account watcher (`social::account_watcher`) lists them again with their new name tag within
//! seconds, as it does for a pp-admin rename.

use axum::extract::{Path, State};
use axum::http::{header, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use common::badges::{check_label, check_png, icon_id, valid_icon_id, valid_icon_name, Place, MAX_BADGES};
use sqlx::PgPool;
use uuid::Uuid;

use crate::AppState;

/// `GET /v1/icons/<id>.png`: an icon. Its address is its content's hash, so it is cached for good.
pub async fn file(State(state): State<AppState>, Path(file): Path<String>) -> Response {
    let Some(id) = file.strip_suffix(".png").filter(|id| valid_icon_id(id)) else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let png: Option<Vec<u8>> =
        match sqlx::query_scalar("SELECT png FROM icons WHERE id = $1").bind(id).fetch_optional(&state.pool).await {
            Ok(png) => png,
            Err(e) => {
                tracing::error!("icon {id}: {e}");
                return StatusCode::INTERNAL_SERVER_ERROR.into_response();
            }
        };
    let Some(png) = png else {
        return StatusCode::NOT_FOUND.into_response();
    };
    (
        [
            (header::CONTENT_TYPE, HeaderValue::from_static("image/png")),
            (header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=31536000, immutable")),
            (header::X_CONTENT_TYPE_OPTIONS, HeaderValue::from_static("nosniff")),
            // The launcher's pages are file:// (an opaque origin); it shows them as <img>, which needs
            // no CORS, but say so anyway for a fetch.
            (header::ACCESS_CONTROL_ALLOW_ORIGIN, HeaderValue::from_static("*")),
        ],
        png,
    )
        .into_response()
}

/// An icon as `pp-admin icon list` shows it.
#[derive(Debug, Clone, sqlx::FromRow)]
pub struct IconRow {
    pub id: String,
    pub name: String,
    pub width: i32,
    pub height: i32,
    pub bytes: i32,
    pub badges: i64,
    pub main_icons: i64,
}

/// Stores a PNG under `name`. The same file under another name is refused (it names that one), as
/// is a name taken by another file. Returns the icon's id and whether it was new (the same file
/// again under its own name changes nothing).
pub async fn add_icon(pool: &PgPool, name: &str, png: &[u8], by: &str) -> anyhow::Result<(String, bool)> {
    if !valid_icon_name(name) {
        anyhow::bail!("icon names are 1-32 of a-z, 0-9 and -: {name:?}");
    }
    let (width, height) = check_png(png).map_err(|e| anyhow::anyhow!("not taken as an icon: {e}"))?;
    let id = icon_id(png);
    let existing: Option<(String, String)> = sqlx::query_as("SELECT id, name FROM icons WHERE id = $1 OR name = $2")
        .bind(&id)
        .bind(name)
        .fetch_optional(pool)
        .await?;
    match existing {
        Some((i, n)) if i == id && n == name => return Ok((id, false)),
        Some((i, n)) if i == id => anyhow::bail!("this file is already the icon {n:?}"),
        Some(_) => anyhow::bail!("there already is an icon called {name:?} (icon remove it first)"),
        None => {}
    }
    sqlx::query("INSERT INTO icons (id, name, png, width, height, uploaded_by) VALUES ($1, $2, $3, $4, $5, $6)")
        .bind(&id)
        .bind(name)
        .bind(png)
        .bind(width as i32)
        .bind(height as i32)
        .bind(by)
        .execute(pool)
        .await?;
    Ok((id, true))
}

pub async fn list_icons(pool: &PgPool) -> sqlx::Result<Vec<IconRow>> {
    sqlx::query_as(
        "SELECT i.id, i.name, i.width, i.height, length(i.png) AS bytes,
                (SELECT count(*) FROM user_badges b WHERE b.icon = i.id) AS badges,
                (SELECT count(*) FROM user_main_icons m WHERE m.icon = i.id) AS main_icons
           FROM icons i ORDER BY i.name",
    )
    .fetch_all(pool)
    .await
}

/// An icon by its name (or its id).
pub async fn find_icon(pool: &PgPool, name: &str) -> anyhow::Result<(String, String)> {
    let row: Option<(String, String)> = sqlx::query_as("SELECT id, name FROM icons WHERE name = $1 OR id = $1")
        .bind(name.trim())
        .fetch_optional(pool)
        .await?;
    row.ok_or_else(|| anyhow::anyhow!("no icon called {name:?} (pp-admin icon list)"))
}

/// Deletes an icon. While players have it (as a badge or main icon) only with `force`, which
/// takes it off them too. Returns the players it was taken off.
pub async fn remove_icon(pool: &PgPool, id: &str, force: bool) -> anyhow::Result<Vec<Uuid>> {
    let mut tx = pool.begin().await?;
    let users: Vec<Uuid> = sqlx::query_scalar(
        "SELECT uid FROM user_badges WHERE icon = $1 UNION SELECT uid FROM user_main_icons WHERE icon = $1",
    )
    .bind(id)
    .fetch_all(&mut *tx)
    .await?;
    if !users.is_empty() && !force {
        anyhow::bail!("{} player(s) have this icon; --force takes it off them too", users.len());
    }
    sqlx::query("DELETE FROM user_badges WHERE icon = $1").bind(id).execute(&mut *tx).await?;
    sqlx::query("DELETE FROM user_main_icons WHERE icon = $1").bind(id).execute(&mut *tx).await?;
    sqlx::query("DELETE FROM icons WHERE id = $1").bind(id).execute(&mut *tx).await?;
    touch(&mut tx, &users).await?;
    tx.commit().await?;
    Ok(users)
}

/// A player's badge as `pp-admin badge list` shows it.
#[derive(Debug, Clone, sqlx::FromRow)]
pub struct BadgeRow {
    pub icon: String,
    pub name: String,
    pub label: String,
    pub pos: i32,
    pub places: Option<Vec<String>>,
}

pub async fn badges_of(pool: &PgPool, uid: Uuid) -> sqlx::Result<Vec<BadgeRow>> {
    sqlx::query_as(
        "SELECT b.icon, i.name, b.label, b.pos, b.places FROM user_badges b JOIN icons i ON i.id = b.icon
          WHERE b.uid = $1 ORDER BY b.pos, b.created_at",
    )
    .bind(uid)
    .fetch_all(pool)
    .await
}

/// A player's main icon: its id, name and hover text.
pub async fn main_icon_of(pool: &PgPool, uid: Uuid) -> sqlx::Result<Option<(String, String, String)>> {
    sqlx::query_as(
        "SELECT m.icon, i.name, m.label FROM user_main_icons m JOIN icons i ON i.id = m.icon WHERE m.uid = $1",
    )
    .bind(uid)
    .fetch_optional(pool)
    .await
}

fn places_column(places: &Option<Vec<Place>>) -> Option<Vec<String>> {
    places.as_ref().map(|p| p.iter().map(|p| p.as_str().to_string()).collect())
}

/// Gives a player a badge, or changes the one they have with that icon. `pos`: 1-based place in
/// their badges (None: last for a new one, unchanged for one they have); `label`/`places`: None
/// keeps what a badge they have says (a new one: no hover text, everywhere).
pub async fn set_badge(
    pool: &PgPool,
    uid: Uuid,
    icon: &str,
    label: Option<&str>,
    places: Option<Option<Vec<Place>>>,
    pos: Option<usize>,
) -> anyhow::Result<()> {
    let label = label.map(check_label).transpose().map_err(|e| anyhow::anyhow!(e))?;
    let mut tx = pool.begin().await?;
    // One change to a player's badges at a time.
    sqlx::query("SELECT uid FROM users WHERE uid = $1 FOR UPDATE").bind(uid).execute(&mut *tx).await?;
    let mut order: Vec<(String, String, Option<Vec<String>>)> =
        sqlx::query_as("SELECT icon, label, places FROM user_badges WHERE uid = $1 ORDER BY pos, created_at")
            .bind(uid)
            .fetch_all(&mut *tx)
            .await?;
    let at = order.iter().position(|(i, _, _)| i == icon);
    let (mut row, was) = match at {
        Some(i) => (order.remove(i), Some(i)),
        None => {
            if order.len() >= MAX_BADGES {
                anyhow::bail!("a player has at most {MAX_BADGES} badges");
            }
            ((icon.to_string(), String::new(), None), None)
        }
    };
    if let Some(l) = label {
        row.1 = l;
    }
    if let Some(p) = &places {
        row.2 = places_column(p);
    }
    let i = match (pos, was) {
        (Some(p), _) => p.saturating_sub(1).min(order.len()),
        (None, Some(w)) => w,
        (None, None) => order.len(),
    };
    order.insert(i, row);
    sqlx::query("DELETE FROM user_badges WHERE uid = $1").bind(uid).execute(&mut *tx).await?;
    for (n, (icon, label, places)) in order.iter().enumerate() {
        sqlx::query("INSERT INTO user_badges (uid, icon, label, pos, places) VALUES ($1, $2, $3, $4, $5)")
            .bind(uid)
            .bind(icon)
            .bind(label)
            .bind(n as i32 + 1)
            .bind(places)
            .execute(&mut *tx)
            .await?;
    }
    touch(&mut tx, &[uid]).await?;
    tx.commit().await?;
    Ok(())
}

/// Takes a badge off a player; false if they didn't have it.
pub async fn remove_badge(pool: &PgPool, uid: Uuid, icon: &str) -> sqlx::Result<bool> {
    let mut tx = pool.begin().await?;
    let gone = sqlx::query("DELETE FROM user_badges WHERE uid = $1 AND icon = $2")
        .bind(uid)
        .bind(icon)
        .execute(&mut *tx)
        .await?
        .rows_affected()
        > 0;
    touch(&mut tx, &[uid]).await?;
    tx.commit().await?;
    Ok(gone)
}

/// Sets (Some: the icon and its hover text, "" for none) or clears (None) a player's main icon.
pub async fn set_main_icon(pool: &PgPool, uid: Uuid, icon: Option<(&str, &str)>) -> anyhow::Result<()> {
    let icon = match icon {
        Some((id, label)) => Some((id, check_label(label).map_err(|e| anyhow::anyhow!(e))?)),
        None => None,
    };
    let mut tx = pool.begin().await?;
    match icon {
        Some((icon, label)) => {
            sqlx::query(
                "INSERT INTO user_main_icons (uid, icon, label) VALUES ($1, $2, $3)
                 ON CONFLICT (uid) DO UPDATE SET icon = $2, label = $3, created_at = now()",
            )
            .bind(uid)
            .bind(icon)
            .bind(label)
            .execute(&mut *tx)
            .await?;
        }
        None => {
            sqlx::query("DELETE FROM user_main_icons WHERE uid = $1").bind(uid).execute(&mut *tx).await?;
        }
    }
    touch(&mut tx, &[uid]).await?;
    tx.commit().await?;
    Ok(())
}

/// Marks the players changed, for the account watcher.
async fn touch(tx: &mut sqlx::Transaction<'_, sqlx::Postgres>, uids: &[Uuid]) -> sqlx::Result<()> {
    if !uids.is_empty() {
        sqlx::query("UPDATE users SET updated_at = now() WHERE uid = ANY($1)").bind(uids).execute(&mut **tx).await?;
    }
    Ok(())
}
