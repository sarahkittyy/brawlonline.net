//! How long a search waits (`pp-admin stats`' table and the launcher's ETA, `GET /v1/rooms`'
//! `queueEta`): mm records every Unranked, Ranked and 2v2 ticket that ends (`queue_searches`);
//! tickets are joined into searches, and each UTC hour of the day gets the wait by which half of
//! its searches were matched, with the searches given up counted as still waiting when they
//! stopped.

use chrono::Duration;
use sqlx::PgPool;

/// Tickets of one player and mode with less than this between one's end and the next one's start
/// are one search: Slippi's client requeues with a new ticket 8-9 s after a match whose P2P
/// connect failed (mm's `FAILED_REQUEUE` is 12 s), and a player who cancels and searches again
/// at once is still after the same game.
pub const QUEUE_REJOIN_SECS: f64 = 12.0;
/// A 2v2 search that started again by itself after its group broke up (`requeue`) joins the
/// search before it from this far apart: the group's ready-up came between them.
pub const TWOS_REQUEUE_JOIN_SECS: f64 = 180.0;

/// One search as `stats` counts it: from its first ticket to the end of its last.
#[derive(Debug, Clone, PartialEq)]
pub struct QueueSearch {
    /// `unranked`, `ranked` or `twos`.
    pub mode: String,
    pub started: chrono::DateTime<chrono::Utc>,
    /// Seconds.
    pub waited: f64,
    /// The last ticket was matched; otherwise the player gave up (cancelled, closed the game).
    pub matched: bool,
}

/// A `queue_searches` row: uid, mode, waited_ms, matched, requeue, ended_at.
pub type QueueSearchRow = (uuid::Uuid, String, i32, bool, bool, chrono::DateTime<chrono::Utc>);

/// Joins tickets into searches ([`QUEUE_REJOIN_SECS`], [`TWOS_REQUEUE_JOIN_SECS`]). `rows` in any
/// order.
pub fn join_queue_searches(mut rows: Vec<QueueSearchRow>) -> Vec<QueueSearch> {
    rows.sort_by(|a, b| (a.0, &a.1, a.5).cmp(&(b.0, &b.1, b.5)));
    let mut out: Vec<QueueSearch> = Vec::new();
    // The open search: its player and mode, its start and the end of its last ticket.
    let mut open: Option<(uuid::Uuid, String, chrono::DateTime<chrono::Utc>, chrono::DateTime<chrono::Utc>)> = None;
    for (uid, mode, waited_ms, matched, requeue, ended) in rows {
        let started = ended - Duration::milliseconds(i64::from(waited_ms.max(0)));
        let joins = open.as_ref().is_some_and(|(u, m, _, end)| {
            let gap = (started - *end).num_milliseconds() as f64 / 1000.0;
            *u == uid && *m == mode && gap <= if requeue { TWOS_REQUEUE_JOIN_SECS } else { QUEUE_REJOIN_SECS }
        });
        let first = match open.take() {
            Some((_, _, s, _)) if joins => {
                out.pop(); // the search goes on: its earlier ticket's end is not its end
                s.min(started)
            }
            _ => started,
        };
        out.push(QueueSearch {
            mode: mode.clone(),
            started: first,
            waited: (ended - first).num_milliseconds() as f64 / 1000.0,
            matched,
        });
        open = Some((uid, mode, first, ended));
    }
    out
}

pub async fn queue_searches(pool: &PgPool, days: i64) -> anyhow::Result<Vec<QueueSearch>> {
    let rows: Vec<QueueSearchRow> = sqlx::query_as(
        "SELECT uid, mode, waited_ms, matched, requeue, ended_at FROM queue_searches
          WHERE ended_at > now() - make_interval(days => $1::int)",
    )
    .bind(days as i32)
    .fetch_all(pool)
    .await?;
    Ok(join_queue_searches(rows))
}

/// The wait by which a share `q` of `searches` (seconds waited, matched) would have been matched,
/// with the searches given up counted as still waiting when they stopped (Kaplan-Meier): an
/// average of the matched ones alone leaves out the players who waited longest and left. None
/// when the share is never reached (too many gave up).
pub fn wait_quantile(searches: &[(f64, bool)], q: f64) -> Option<f64> {
    let mut s: Vec<(f64, bool)> = searches.to_vec();
    // At the same wait, matches before give-ups (the usual Kaplan-Meier order).
    s.sort_by(|a, b| a.0.total_cmp(&b.0).then(b.1.cmp(&a.1)));
    let n = s.len();
    let mut waiting = 1.0;
    for (i, (w, matched)) in s.into_iter().enumerate() {
        if matched {
            waiting *= 1.0 - 1.0 / (n - i) as f64;
            if 1.0 - waiting >= q - 1e-9 {
                return Some(w);
            }
        }
    }
    None
}

/// One hour of the day (UTC) of one mode, over the days `stats` reads.
#[derive(Debug, Clone, PartialEq)]
pub struct HourWait {
    pub searches: usize,
    pub matched: usize,
    /// The median wait ([`wait_quantile`] 0.5): the estimate.
    pub median: Option<f64>,
    /// 3 in 4 searches were matched by then.
    pub p75: Option<f64>,
}

/// `searches` of `mode` by the UTC hour they started in.
pub fn hourly_waits(searches: &[QueueSearch], mode: &str) -> Vec<HourWait> {
    use chrono::Timelike;
    (0..24)
        .map(|h| {
            let s: Vec<(f64, bool)> = searches
                .iter()
                .filter(|s| s.mode == mode && s.started.hour() == h)
                .map(|s| (s.waited, s.matched))
                .collect();
            HourWait {
                searches: s.len(),
                matched: s.iter().filter(|x| x.1).count(),
                median: wait_quantile(&s, 0.5),
                p75: wait_quantile(&s, 0.75),
            }
        })
        .collect()
}

/// A median from fewer searches than this is not an estimate (`stats` prints `-`, `queueEta` is
/// null).
pub const MIN_SEARCHES: usize = 5;
/// The launcher's ETA says "10+ min" from this wait on (`queueEta` null), in seconds.
pub const ETA_MAX_SECS: f64 = 600.0;
/// The days `queueEta` reads (`stats`' default).
pub const ETA_DAYS: i64 = 14;
/// How often accounts reads the searches again for `queueEta`.
pub const ETA_EVERY: std::time::Duration = std::time::Duration::from_secs(600);

/// An hour's ETA in whole seconds, as `stats` prints its ETA column (cut to the second): `None`
/// ("10+ min") when most of its searches gave up (`stats`' "never"), the median is
/// [`ETA_MAX_SECS`] or more, or the hour had under [`MIN_SEARCHES`] searches.
pub fn hour_eta(w: &HourWait) -> Option<u32> {
    if w.searches < MIN_SEARCHES {
        return None;
    }
    w.median.filter(|&s| s < ETA_MAX_SECS).map(|s| s as u32)
}

/// Unranked's and Ranked's ETA for every UTC hour of the day ([`hour_eta`]).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct EtaTable {
    pub unranked: [Option<u32>; 24],
    pub ranked: [Option<u32>; 24],
}

impl EtaTable {
    pub fn of(searches: &[QueueSearch]) -> EtaTable {
        let hours = |mode: &str| {
            let mut v = [None; 24];
            for (h, w) in hourly_waits(searches, mode).iter().enumerate() {
                v[h] = hour_eta(w);
            }
            v
        };
        EtaTable { unranked: hours("unranked"), ranked: hours("ranked") }
    }

    /// The ETA of the UTC hour `now` is in.
    pub fn at(&self, now: chrono::DateTime<chrono::Utc>) -> common::rooms::QueueEta {
        use chrono::Timelike;
        let h = now.hour() as usize % 24;
        common::rooms::QueueEta { unranked: self.unranked[h], ranked: self.ranked[h] }
    }
}

/// accounts' [`EtaTable`] of the last [`ETA_DAYS`], read again every [`ETA_EVERY`] by
/// [`eta_watcher`], never by a request: `GET /v1/rooms` only picks the current hour from it.
#[derive(Default)]
pub struct EtaCache {
    table: std::sync::RwLock<Option<EtaTable>>,
}

impl EtaCache {
    /// The current hour's ETA; `None` until the searches were first read.
    pub fn now(&self, now: chrono::DateTime<chrono::Utc>) -> Option<common::rooms::QueueEta> {
        self.table.read().unwrap_or_else(|e| e.into_inner()).as_ref().map(|t| t.at(now))
    }

    pub fn set(&self, table: EtaTable) {
        *self.table.write().unwrap_or_else(|e| e.into_inner()) = Some(table);
    }

    /// Reads the searches and replaces the table.
    pub async fn refresh(&self, pool: &PgPool) -> anyhow::Result<()> {
        let searches = queue_searches(pool, ETA_DAYS).await?;
        self.set(EtaTable::of(&searches));
        Ok(())
    }
}

/// Refreshes `cache` now and then every [`ETA_EVERY`]; a failed read keeps the last table.
pub async fn eta_watcher(pool: PgPool, cache: std::sync::Arc<EtaCache>) {
    loop {
        match tokio::time::timeout(std::time::Duration::from_secs(120), cache.refresh(&pool)).await {
            Ok(Ok(())) => {}
            Ok(Err(e)) => tracing::warn!("reading the queue searches for queueEta failed: {e}"),
            Err(_) => tracing::warn!("reading the queue searches for queueEta timed out"),
        }
        tokio::time::sleep(ETA_EVERY).await;
    }
}

#[cfg(test)]
mod queue_tests {
    use super::*;
    use chrono::{TimeZone, Utc};

    fn at(secs: i64) -> chrono::DateTime<Utc> {
        Utc.with_ymd_and_hms(2026, 11, 9, 20, 0, 0).unwrap() + Duration::seconds(secs)
    }

    #[test]
    fn a_failed_connect_requeue_is_one_search() {
        let a = uuid::Uuid::from_u128(1);
        let b = uuid::Uuid::from_u128(2);
        let rows = vec![
            // a: matched after 30 s at t=30, the connect failed, searched again at 39, matched at 59.
            (a, "ranked".into(), 30_000, true, false, at(30)),
            (a, "ranked".into(), 20_000, true, false, at(59)),
            // a again 10 minutes later (after a game): a new search.
            (a, "ranked".into(), 5_000, true, false, at(660)),
            // b in Unranked at the same time: its own search; gave up after 90 s.
            (b, "unranked".into(), 90_000, false, false, at(100)),
            // a 2v2 group that broke up: the requeue a minute later joins it.
            (b, "twos".into(), 40_000, true, false, at(1000)),
            (b, "twos".into(), 15_000, true, true, at(1075)),
        ];
        let s = join_queue_searches(rows);
        let got: Vec<(&str, f64, bool)> = s.iter().map(|s| (s.mode.as_str(), s.waited, s.matched)).collect();
        assert_eq!(
            got,
            vec![("ranked", 59.0, true), ("ranked", 5.0, true), ("twos", 115.0, true), ("unranked", 90.0, false)]
        );
        assert_eq!(s[0].started, at(0));
    }

    #[test]
    fn a_replaced_ticket_overlapping_the_next_is_one_search() {
        let a = uuid::Uuid::from_u128(1);
        // The newer ticket started at 50 and ended the older (started at 0) at 50.5.
        let s = join_queue_searches(vec![
            (a, "unranked".into(), 50_500, false, false, at(50) + Duration::milliseconds(500)),
            (a, "unranked".into(), 20_000, true, false, at(70)),
        ]);
        assert_eq!(s.len(), 1);
        assert_eq!((s[0].waited, s[0].matched), (70.0, true));
    }

    #[test]
    fn give_ups_count_as_still_waiting() {
        // Matched at 10, 20, 30; two left at 5 s and two at 100 s.
        let s = [(10.0, true), (20.0, true), (30.0, true), (5.0, false), (5.0, false), (100.0, false), (100.0, false)];
        // Of the 5 still there after 5 s, 3 are matched by 30 s: 60%.
        assert_eq!(wait_quantile(&s, 0.2), Some(10.0));
        assert_eq!(wait_quantile(&s, 0.5), Some(30.0));
        assert_eq!(wait_quantile(&s, 0.75), None);
        assert_eq!(wait_quantile(&[], 0.5), None);
        // The matched alone would say 20 s.
        assert_eq!(wait_quantile(&[(10.0, true), (20.0, true), (30.0, true)], 0.5), Some(20.0));
    }

    #[test]
    fn the_eta_is_the_hours_median_unless_most_gave_up_or_it_is_long_or_few() {
        let hour = |h: u32, secs: i64| Utc.with_ymd_and_hms(2026, 11, 9, h, 0, 0).unwrap() + Duration::seconds(secs);
        let search = |mode: &str, h: u32, waited: f64, matched: bool| QueueSearch {
            mode: mode.into(),
            started: hour(h, 0),
            waited,
            matched,
        };
        let mut s = Vec::new();
        // 21 UTC: Unranked matched at 10..50 s (median 30); in Ranked 3 of 5 gave up ("never").
        for w in [10.0, 20.0, 30.0, 40.0, 50.0] {
            s.push(search("unranked", 21, w, true));
        }
        for (w, m) in [(10.0, true), (20.0, true), (30.0, false), (40.0, false), (50.0, false)] {
            s.push(search("ranked", 21, w, m));
        }
        // 3 UTC: Unranked has under 5 searches; Ranked's median is 10 minutes.
        for w in [5.0, 6.0, 7.0, 8.0] {
            s.push(search("unranked", 3, w, true));
        }
        for w in [500.0, 550.0, 600.0, 650.0, 700.0] {
            s.push(search("ranked", 3, w, true));
        }
        // 4 UTC: Ranked's median just under 10 minutes, cut to the second.
        for w in [590.0, 599.9, 599.9, 650.0, 700.0] {
            s.push(search("ranked", 4, w, true));
        }
        let t = EtaTable::of(&s);
        let at = |h: u32| t.at(hour(h, 1800));
        assert_eq!(at(21), common::rooms::QueueEta { unranked: Some(30), ranked: None });
        assert_eq!(at(3), common::rooms::QueueEta { unranked: None, ranked: None });
        assert_eq!(at(4).ranked, Some(599));
        assert_eq!(at(12), common::rooms::QueueEta::default(), "no searches");
        assert_eq!(serde_json::to_value(at(21)).unwrap(), serde_json::json!({"unranked": 30, "ranked": null}));

        let cache = EtaCache::default();
        assert_eq!(cache.now(hour(21, 0)), None, "not read yet");
        cache.set(t);
        assert_eq!(cache.now(hour(21, 0)).unwrap().unranked, Some(30));
    }
}
