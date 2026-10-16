//! A player's match history (`GET /v1/me/matches`): their Ranked, Unranked and Direct matches and
//! their rooms' games (1v1, free-for-all, teams), newest first, with the score from the game
//! reports and, for ranked sets, the rating change.
//!
//! mm records every match it makes (`mm_matches`), including the many that never start (a
//! failed P2P connect requeues with a new match). The history shows a match once something
//! was played or decided in it: it has a game report, or it is a ranked set that was settled
//! and either changed a rating or has a leave report (an abandoned set with no game). A ranked
//! set that went silent without any report (`ORPHANED`, nobody rated) is left out.
//!
//! A room's game (mode 3, `migrations/0007_room_matches.sql`) is reported by every player's game
//! with each player's place (1 = first, shared by a tie and by a team). The places count when
//! more than half of the players' reports of that game agree on them ([`room_places`]).
//!
//! Keyset pagination on (`created_at`, `match_id`), newest first; the cursor is the last row's.

use std::collections::{BTreeMap, HashMap};

use axum::extract::{Query, State};
use axum::http::HeaderMap;
use axum::Json;
use chrono::{DateTime, Utc};
use common::ranked::{self, GameReport, Timing, LEFT_BEFORE_STAGE_SELECT};
use serde::{Deserialize, Serialize};
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{current_user, limit};
use crate::cursor;
use crate::error::{ApiError, ApiResult};
use crate::ranked::MODE_RANKED;
use crate::AppState;

pub const DEFAULT_LIMIT: u32 = 20;
pub const MAX_LIMIT: u32 = 50;

/// `mm_matches.mode` of a room's game (Slippi's Teams, the room ticket's mode).
pub const MODE_ROOM: i16 = 3;

/// The `mode` query parameter.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModeFilter {
    All,
    Ranked,
    /// Unranked and Direct.
    Unranked,
    /// Rooms' games.
    Rooms,
}

impl ModeFilter {
    pub fn parse(s: Option<&str>) -> Option<ModeFilter> {
        match s.map(str::trim).unwrap_or("") {
            "" | "all" => Some(ModeFilter::All),
            "ranked" => Some(ModeFilter::Ranked),
            "unranked" => Some(ModeFilter::Unranked),
            "rooms" => Some(ModeFilter::Rooms),
            _ => None,
        }
    }

    fn sql(self) -> &'static str {
        match self {
            ModeFilter::All => "",
            ModeFilter::Ranked => " AND m.mode = 0",
            ModeFilter::Unranked => " AND m.mode IN (1, 2)",
            ModeFilter::Rooms => " AND m.mode = 3",
        }
    }
}

/// `mm_matches.mode` as the API names it (Slippi's mode numbers: 0 Ranked, 1 Unranked,
/// 2 Direct, 3 a room's game).
pub fn mode_name(mode: i16) -> &'static str {
    match mode {
        0 => "ranked",
        2 => "direct",
        MODE_ROOM => "room",
        _ => "unranked",
    }
}

/// One player's report of one room game: each player's place (1 = first).
#[derive(Debug, Clone, PartialEq)]
pub struct PlaceReport {
    pub game_index: u32,
    pub reporter: Uuid,
    pub places: BTreeMap<Uuid, u32>,
}

impl PlaceReport {
    /// The places in a report's `players` (`[{uid, place, ...}]`): entries whose uid is one of
    /// `players` and whose place is 1-4. A player who left the game has none.
    pub fn parse(game_index: u32, reporter: Uuid, detail_players: &serde_json::Value, players: &[Uuid]) -> PlaceReport {
        let places = detail_players
            .as_array()
            .map(Vec::as_slice)
            .unwrap_or_default()
            .iter()
            .filter_map(|p| {
                let uid = Uuid::parse_str(p.get("uid")?.as_str()?).ok().filter(|u| players.contains(u))?;
                let place = p.get("place")?.as_u64().filter(|n| (1..=4).contains(n))?;
                Some((uid, place as u32))
            })
            .collect();
        PlaceReport { game_index, reporter, places }
    }
}

/// The places of each game of a room's match, by game index: what more than half of the players'
/// reports of that game say. Reports from anyone but the players, and reports with no places,
/// count for nothing; a game whose reports split evenly has no places.
pub fn room_places(players: &[Uuid], reports: &[PlaceReport]) -> BTreeMap<u32, BTreeMap<Uuid, u32>> {
    let mut by_game: BTreeMap<u32, Vec<&BTreeMap<Uuid, u32>>> = BTreeMap::new();
    for r in reports.iter().filter(|r| players.contains(&r.reporter) && !r.places.is_empty()) {
        by_game.entry(r.game_index).or_default().push(&r.places);
    }
    by_game
        .into_iter()
        .filter_map(|(game, all)| {
            let agreed = all.iter().find(|p| all.iter().filter(|q| q == p).count() * 2 > all.len())?;
            Some((game, (*agreed).clone()))
        })
        .collect()
}

/// Games won per player of a room's match (in the order of `players`): a first place in a game
/// where someone placed lower (a draw, everyone first, counts for nobody, as in a 1v1).
pub fn room_wins(players: &[Uuid], games: &BTreeMap<u32, BTreeMap<Uuid, u32>>) -> Vec<u32> {
    players
        .iter()
        .map(|p| games.values().filter(|g| g.get(p) == Some(&1) && g.values().any(|place| *place > 1)).count() as u32)
        .collect()
}

/// Games won per player, in the order of `players`: [`ranked::resolve`]'s count for a ranked
/// set (the rules that settle it), [`ranked::count_wins`] for the other modes.
pub fn wins(
    mode: i16,
    players: &[Uuid],
    created: DateTime<Utc>,
    games: &[GameReport],
    now: DateTime<Utc>,
    timing: Timing,
) -> Vec<u32> {
    if mode == MODE_RANKED {
        if let Ok(p) = <[Uuid; 2]>::try_from(players) {
            return ranked::resolve(p, created, games, &[], now, timing).wins().to_vec();
        }
    }
    ranked::count_wins(players, games)
}

/// Where a page starts: after (older than) this match.
#[derive(Debug, Clone, PartialEq)]
pub struct Key {
    pub created_at: DateTime<Utc>,
    pub match_id: String,
}

impl Key {
    pub fn cursor(&self) -> String {
        cursor::encode("h1", &[&self.created_at.timestamp_micros().to_string(), &self.match_id])
    }

    pub fn parse(c: &str) -> Option<Key> {
        let f = cursor::decode("h1", c, 2)?;
        let created_at = DateTime::from_timestamp_micros(f[0].parse().ok()?)?;
        Some(Key { created_at, match_id: f[1].clone() })
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchPlayer {
    pub uid: Uuid,
    /// Empty for a deleted account.
    pub display_name: String,
    pub connect_code: String,
    /// Games won in this match.
    pub wins: u32,
    /// The rating change this set caused; null for Unranked and Direct, and for a set that
    /// did not change this player's rating (undecided, void, or abandoned by the opponent
    /// before the first game).
    pub rating_before: Option<f64>,
    pub rating_after: Option<f64>,
    pub rating_change: Option<f64>,
    /// A room's game: the player's slot (1-4, the in-game port); null for the other modes.
    pub port: Option<u8>,
    /// A room's team battle: 0 red, 1 blue, 2 green.
    pub team: Option<u8>,
    /// A room's game: the place in its last game whose reports agree (1 = first, shared by a tie
    /// and by a team); null until then, and for a player who left the game.
    pub place: Option<u32>,
}

/// The room a game was played in.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchRoom {
    pub code: String,
    /// `1v1`, `ffa` or `teams`.
    pub mode: String,
    /// The published ruleset it played (its page: `/r/<code>`); null for P+'s default, custom
    /// rules and games from before rulesets were recorded.
    pub ruleset: Option<MatchRuleset>,
}

/// A room game's published ruleset, as it was when played.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchRuleset {
    pub code: String,
    pub name: String,
    pub version: u32,
    /// Its page on the website (`<PUBLIC_BASE_URL>/r/<code>`).
    pub link: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchItem {
    pub match_id: String,
    /// `ranked`, `unranked`, `direct` or `room`.
    pub mode: &'static str,
    pub created_at: DateTime<Utc>,
    /// The `mm_matches` status: `ASSIGNED` while a ranked set is undecided (and always for
    /// Unranked and Direct), then `COMPLETE`, `ABANDONED`, `TERMINATED`, `ERROR` or `ORPHANED`.
    pub status: String,
    pub ranked: bool,
    pub players: Vec<MatchPlayer>,
    /// The set's winner (ranked only).
    pub winner: Option<Uuid>,
    pub end_reason: Option<String>,
    /// A room's game: the room; null for the other modes.
    pub room: Option<MatchRoom>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MatchPage {
    pub matches: Vec<MatchItem>,
    /// The cursor for the next (older) page (`before`), or null on the last page.
    pub next: Option<String>,
}

/// A `game_reports` row: match, game, reporter, winner, time.
type ReportRow = (String, i32, Uuid, Option<Uuid>, DateTime<Utc>);

#[derive(Debug, sqlx::FromRow)]
struct Row {
    match_id: String,
    mode: i16,
    created_at: DateTime<Utc>,
    status: String,
    players: Vec<Uuid>,
    winner: Option<Uuid>,
    end_reason: Option<String>,
    room_code: Option<String>,
    room_mode: Option<String>,
    ports: Option<Vec<i16>>,
    teams: Option<Vec<i16>>,
    ruleset_code: Option<String>,
    ruleset_name: Option<String>,
    ruleset_version: Option<i32>,
}

/// One page of `uid`'s matches older than `before` (newest first).
pub async fn matches(
    pool: &PgPool,
    uid: Uuid,
    mode: ModeFilter,
    before: Option<&Key>,
    limit: u32,
    now: DateTime<Utc>,
    timing: Timing,
) -> sqlx::Result<MatchPage> {
    let limit = i64::from(limit);
    let mut tx = pool.begin().await?;
    sqlx::query("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY").execute(&mut *tx).await?;
    let sql = format!(
        "SELECT m.match_id, m.mode, m.created_at, m.status, m.players, m.winner, m.end_reason,
                m.room_code, m.room_mode, m.ports, m.teams, m.ruleset_code, m.ruleset_name, m.ruleset_version
           FROM mm_matches m
          WHERE m.players @> ARRAY[$1]::uuid[]{}
            AND ($2::timestamptz IS NULL OR (m.created_at, m.match_id) < ($2, $3))
            AND (EXISTS (SELECT 1 FROM game_reports g WHERE g.match_id = m.match_id)
                 OR (m.mode = {MODE_RANKED} AND m.status <> 'ASSIGNED'
                     AND m.end_reason IS DISTINCT FROM '{LEFT_BEFORE_STAGE_SELECT}'
                     AND (EXISTS (SELECT 1 FROM rating_events e WHERE e.match_id = m.match_id)
                          OR EXISTS (SELECT 1 FROM leave_reports l WHERE l.match_id = m.match_id))))
          ORDER BY m.created_at DESC, m.match_id DESC
          LIMIT $4",
        mode.sql()
    );
    let mut rows: Vec<Row> = sqlx::query_as(&sql)
        .bind(uid)
        .bind(before.map(|k| k.created_at))
        .bind(before.map(|k| k.match_id.clone()))
        .bind(limit + 1)
        .fetch_all(&mut *tx)
        .await?;
    let more = rows.len() as i64 > limit;
    rows.truncate(limit as usize);
    let next = if more {
        rows.last().map(|r| Key { created_at: r.created_at, match_id: r.match_id.clone() }.cursor())
    } else {
        None
    };

    let ids: Vec<String> = rows.iter().map(|r| r.match_id.clone()).collect();
    let reports: Vec<ReportRow> = sqlx::query_as(
        "SELECT match_id, game_index, reporter, winner, created_at FROM game_reports WHERE match_id = ANY($1)",
    )
    .bind(&ids)
    .fetch_all(&mut *tx)
    .await?;
    let room_ids: Vec<&str> = rows.iter().filter(|r| r.mode == MODE_ROOM).map(|r| r.match_id.as_str()).collect();
    let room_reports: Vec<(String, i32, Uuid, Option<serde_json::Value>)> = if room_ids.is_empty() {
        Vec::new()
    } else {
        sqlx::query_as(
            "SELECT match_id, game_index, reporter, detail -> 'players' FROM game_reports WHERE match_id = ANY($1)",
        )
        .bind(&room_ids)
        .fetch_all(&mut *tx)
        .await?
    };
    let events: Vec<(String, Uuid, f64, f64)> =
        sqlx::query_as("SELECT match_id, uid, rating_before, rating_after FROM rating_events WHERE match_id = ANY($1)")
            .bind(&ids)
            .fetch_all(&mut *tx)
            .await?;
    let mut uids: Vec<Uuid> = rows.iter().flat_map(|r| r.players.iter().copied()).collect();
    uids.sort();
    uids.dedup();
    let users: Vec<(Uuid, String, Option<String>)> =
        sqlx::query_as("SELECT uid, display_name, connect_code FROM users WHERE uid = ANY($1)")
            .bind(&uids)
            .fetch_all(&mut *tx)
            .await?;
    tx.commit().await?;

    let mut games: HashMap<String, Vec<GameReport>> = HashMap::new();
    for (id, g, reporter, winner, at) in reports {
        games.entry(id).or_default().push(GameReport { game_index: g.max(0) as u32, reporter, winner, at });
    }
    let mut place_reports: HashMap<String, Vec<(u32, Uuid, serde_json::Value)>> = HashMap::new();
    for (id, g, reporter, players) in room_reports {
        place_reports.entry(id).or_default().push((g.max(0) as u32, reporter, players.unwrap_or_default()));
    }
    let events: HashMap<(String, Uuid), (f64, f64)> =
        events.into_iter().map(|(id, u, before, after)| ((id, u), (before, after))).collect();
    let users: HashMap<Uuid, (String, Option<String>)> =
        users.into_iter().map(|(u, name, code)| (u, (name, code))).collect();

    let matches = rows
        .into_iter()
        .map(|r| {
            let is_room = r.mode == MODE_ROOM;
            let places = if is_room {
                let reports: Vec<PlaceReport> = place_reports
                    .get(&r.match_id)
                    .map_or(&[][..], Vec::as_slice)
                    .iter()
                    .map(|(g, reporter, players)| PlaceReport::parse(*g, *reporter, players, &r.players))
                    .collect();
                room_places(&r.players, &reports)
            } else {
                BTreeMap::new()
            };
            let won = if is_room {
                room_wins(&r.players, &places)
            } else {
                wins(r.mode, &r.players, r.created_at, games.get(&r.match_id).map_or(&[], Vec::as_slice), now, timing)
            };
            let last = places.values().next_back();
            // The room's slots and teams, one per player.
            let nth = |v: &Option<Vec<i16>>, i: usize| v.as_ref()?.get(i).and_then(|n| u8::try_from(*n).ok());
            let players = r
                .players
                .iter()
                .zip(won)
                .enumerate()
                .map(|(i, (p, wins))| {
                    let (name, code) = users.get(p).cloned().unwrap_or_default();
                    let event = events.get(&(r.match_id.clone(), *p)).copied();
                    MatchPlayer {
                        uid: *p,
                        display_name: name,
                        connect_code: code.unwrap_or_default(),
                        wins,
                        rating_before: event.map(|e| e.0),
                        rating_after: event.map(|e| e.1),
                        rating_change: event.map(|(before, after)| after - before),
                        port: nth(&r.ports, i),
                        team: nth(&r.teams, i),
                        place: last.and_then(|g| g.get(p).copied()),
                    }
                })
                .collect();
            let room = match (is_room, r.room_code) {
                (true, Some(code)) => Some(MatchRoom {
                    code,
                    mode: r.room_mode.unwrap_or_default(),
                    ruleset: r.ruleset_code.map(|code| MatchRuleset {
                        code,
                        name: r.ruleset_name.unwrap_or_default(),
                        version: r.ruleset_version.and_then(|v| u32::try_from(v).ok()).unwrap_or(0),
                        link: String::new(),
                    }),
                }),
                _ => None,
            };
            MatchItem {
                mode: mode_name(r.mode),
                ranked: r.mode == MODE_RANKED,
                match_id: r.match_id,
                created_at: r.created_at,
                status: r.status,
                players,
                winner: r.winner,
                end_reason: r.end_reason,
                room,
            }
        })
        .collect();
    Ok(MatchPage { matches, next })
}

#[derive(Debug, Deserialize)]
pub struct MatchesQuery {
    pub mode: Option<String>,
    pub limit: Option<u32>,
    pub before: Option<String>,
}

/// `GET /v1/me/matches?mode=all|ranked|unranked|rooms&limit=&before=`. Session auth; limited per
/// account ([`common::ratelimit::HISTORY_WINDOWS`]).
pub async fn my_matches(
    State(state): State<AppState>,
    headers: HeaderMap,
    Query(q): Query<MatchesQuery>,
) -> ApiResult<Json<MatchPage>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.history.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let mode = ModeFilter::parse(q.mode.as_deref())
        .ok_or_else(|| ApiError::bad_request("invalid_mode", "mode must be all, ranked, unranked or rooms"))?;
    let before = match q.before.as_deref().filter(|b| !b.is_empty()) {
        None => None,
        Some(b) => Some(Key::parse(b).ok_or_else(|| ApiError::bad_request("invalid_cursor", "Invalid cursor"))?),
    };
    let n = cursor::page_limit(q.limit, DEFAULT_LIMIT, MAX_LIMIT);
    let mut page =
        matches(&state.pool, user.uid, mode, before.as_ref(), n, Utc::now(), state.cfg.ranked_timing()).await?;
    let base = state.cfg.public_base_url.trim_end_matches('/');
    for r in page.matches.iter_mut().filter_map(|m| m.room.as_mut()?.ruleset.as_mut()) {
        r.link = format!("{base}/r/{}", r.code);
    }
    Ok(Json(page))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn modes() {
        assert_eq!(ModeFilter::parse(None), Some(ModeFilter::All));
        assert_eq!(ModeFilter::parse(Some("all")), Some(ModeFilter::All));
        assert_eq!(ModeFilter::parse(Some("ranked")), Some(ModeFilter::Ranked));
        assert_eq!(ModeFilter::parse(Some("unranked")), Some(ModeFilter::Unranked));
        assert_eq!(ModeFilter::parse(Some("rooms")), Some(ModeFilter::Rooms));
        assert_eq!(ModeFilter::parse(Some("direct")), None);
        assert_eq!((mode_name(0), mode_name(1), mode_name(2), mode_name(3)), ("ranked", "unranked", "direct", "room"));
    }

    fn uids(n: u8) -> Vec<Uuid> {
        (1..=n).map(|i| Uuid::from_bytes([i; 16])).collect()
    }

    fn report(game: u32, reporter: Uuid, places: &[(Uuid, u32)]) -> PlaceReport {
        PlaceReport { game_index: game, reporter, places: places.iter().copied().collect() }
    }

    #[test]
    fn room_reports_give_the_players_places_only() {
        let p = uids(3);
        let stranger = Uuid::from_bytes([9; 16]);
        let detail = serde_json::json!([
            {"uid": p[0].to_string(), "port": 1, "place": 2, "characterId": 3},
            {"uid": p[1].to_string(), "port": 2, "place": 1},
            {"uid": p[2].to_string(), "port": 3, "left": true},
            {"uid": stranger.to_string(), "place": 3},
            {"uid": p[2].to_string(), "place": 7},
            "junk",
        ]);
        let r = PlaceReport::parse(1, p[0], &detail, &p);
        assert_eq!(r.places, [(p[0], 2), (p[1], 1)].into_iter().collect());
        assert!(PlaceReport::parse(1, p[0], &serde_json::json!({"x": 1}), &p).places.is_empty());
        assert!(PlaceReport::parse(1, p[0], &serde_json::Value::Null, &p).places.is_empty());
    }

    #[test]
    fn room_places_take_what_most_reports_say() {
        let p = uids(4);
        let a = [(p[0], 1), (p[1], 2), (p[2], 3), (p[3], 4)];
        let b = [(p[0], 2), (p[1], 1), (p[2], 3), (p[3], 4)];
        let want: BTreeMap<Uuid, u32> = a.into_iter().collect();
        // One report is enough; agreeing reports too.
        assert_eq!(room_places(&p, &[report(1, p[0], &a)])[&1], want);
        let mut all: Vec<_> = p.iter().map(|r| report(1, *r, &a)).collect();
        assert_eq!(room_places(&p, &all)[&1], want);
        // Three against one: the three. Two against two: nothing.
        all[3] = report(1, p[3], &b);
        assert_eq!(room_places(&p, &all)[&1], want);
        all[2] = report(1, p[2], &b);
        assert!(room_places(&p, &all).is_empty());
        // A stranger's reports and an empty one count for nothing.
        let stranger = Uuid::from_bytes([9; 16]);
        let r = [report(1, p[0], &a), report(1, stranger, &b), report(1, stranger, &b), report(1, p[1], &[])];
        assert_eq!(room_places(&p, &r)[&1], want);
        // Games are separate.
        let r = [report(1, p[0], &a), report(2, p[0], &b)];
        assert_eq!(room_places(&p, &r).len(), 2);
    }

    #[test]
    fn room_wins_count_first_places_someone_placed_under() {
        let p = uids(4);
        let mut games = BTreeMap::new();
        // Free-for-all: p0 first.
        games.insert(1, [(p[0], 1), (p[1], 3), (p[2], 2), (p[3], 4)].into_iter().collect());
        assert_eq!(room_wins(&p, &games), vec![1, 0, 0, 0]);
        // Teams: p1 and p3 share first place, both won.
        games.insert(2, [(p[0], 2), (p[1], 1), (p[2], 2), (p[3], 1)].into_iter().collect());
        assert_eq!(room_wins(&p, &games), vec![1, 1, 0, 1]);
        // A draw (everyone left first) counts for nobody; p3 left and has no place.
        games.insert(3, [(p[0], 1), (p[1], 1), (p[2], 1)].into_iter().collect());
        assert_eq!(room_wins(&p, &games), vec![1, 1, 0, 1]);
    }

    #[test]
    fn cursors_keep_the_microseconds() {
        let t = DateTime::from_timestamp_micros(1_791_460_800_123_456).unwrap();
        let k = Key { created_at: t, match_id: "mode.ranked-1791460800-7".into() };
        assert_eq!(Key::parse(&k.cursor()).unwrap(), k);
        assert!(Key::parse(&cursor::encode("h1", &["soon", "x"])).is_none());
        assert!(Key::parse(&cursor::encode("l1", &["1", "x"])).is_none());
    }

    #[test]
    fn ranked_wins_wait_for_both_reports_and_others_do_not() {
        let p = [Uuid::from_bytes([1; 16]), Uuid::from_bytes([2; 16])];
        let t0 = DateTime::from_timestamp(1_791_460_800, 0).unwrap();
        let lone = [GameReport { game_index: 1, reporter: p[0], winner: Some(p[0]), at: t0 }];
        let timing = Timing::default();
        // Ranked: a lone report waits for the grace period.
        assert_eq!(wins(0, &p, t0, &lone, t0, timing), vec![0, 0]);
        assert_eq!(wins(0, &p, t0, &lone, t0 + timing.report_grace, timing), vec![1, 0]);
        // Unranked and Direct count it at once.
        assert_eq!(wins(1, &p, t0, &lone, t0, timing), vec![1, 0]);
        assert_eq!(wins(2, &p, t0, &lone, t0, timing), vec![1, 0]);
    }
}
