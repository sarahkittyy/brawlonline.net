//! How fast players' PCs run the game (`docs/design/speed-check.md`, 2026-10-22). A PC that cannot
//! hold 60 frames a second slows the whole online game: under rollback the other player's game runs
//! at its speed (GameplaySession's "frames at the peer's speed"). Two sources:
//!
//! - every online game: each player's own game says how fast it ran (`spectate-end`'s `perf`, from
//!   the replay's `ENDG`), kept in `speed_reports`;
//! - the waiting room: while an Unranked or Ranked search waits in Brawl's Online Training room,
//!   the game measures its frame rate in 300-frame windows and sends `speed-check`, kept in
//!   `speed_checks`.
//!
//! The rule ([`locked`]): two slow Ranked games in a row lock the player out of Ranked until a
//! waiting-room check passes. For now it is log only: mm records, `pp-admin speed` shows who it
//! would lock, and nothing is refused.

use serde::{Deserialize, Deserializer};

pub const SPEED_CHECK: &str = "speed-check";

/// Under this many frames a second a PC is slow (the game's own "can't keep up" notice,
/// GameplaySession's SLOW_FPS).
pub const SLOW_FPS: f64 = 58.0;
/// A game shorter than this (10 s) says nothing.
pub const MIN_GAME_FRAMES: u32 = 600;
/// A slow game's share of frames late for this PC's own reasons, not after a rollback (the
/// game's LogPerfVerdict): more than half.
pub const SLOW_OWN_LATE: f64 = 0.5;
/// A waiting-room check needs this many 300-frame windows to say anything.
pub const MIN_CHECK_WINDOWS: usize = 2;
pub const MAX_CHECK_WINDOWS: usize = 4;
/// Slow Ranked games in a row that lock the player out of Ranked.
pub const LOCK_GAMES: u32 = 2;

/// `spectate-end`'s `perf`: this PC's speed over the game, as its own game measured it.
#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GamePerf {
    /// Displayed frames the game ran.
    pub frames: u32,
    pub fps: f64,
    /// The share of frames late without a rollback before them: this PC's own speed (CPU or GPU).
    pub own_late: f64,
    /// The share of frames run at the speed of a peer that could not keep up ...
    pub followed: f64,
    /// ... and that peer's frame rate over them (left out when none).
    #[serde(default)]
    pub peer_fps: Option<f64>,
    /// The game showed "Your PC can't keep up" / "<name>'s PC can't keep up".
    #[serde(default)]
    pub slow: bool,
    #[serde(default)]
    pub peer_slow: bool,
}

impl GamePerf {
    fn valid(&self) -> bool {
        let share = |v: f64| v.is_finite() && (0.0..=1.0).contains(&v);
        let rate = |v: f64| v.is_finite() && (0.0..=1000.0).contains(&v);
        self.frames <= 10_000_000
            && rate(self.fps)
            && share(self.own_late)
            && share(self.followed)
            && self.peer_fps.is_none_or(rate)
    }

    /// Whether this PC held the game back: long enough to tell, under [`SLOW_FPS`], and more than
    /// half its frames late for its own reasons. A game slowed by the other player's PC is not.
    pub fn slow_game(&self) -> bool {
        self.frames >= MIN_GAME_FRAMES && self.fps < SLOW_FPS && self.own_late > SLOW_OWN_LATE
    }
}

/// For `perf` in a message whose other fields matter more: anything that does not read as a
/// valid [`GamePerf`] is none, never an error.
pub fn lenient_perf<'de, D: Deserializer<'de>>(d: D) -> Result<Option<GamePerf>, D::Error> {
    let v = Option::<serde_json::Value>::deserialize(d)?;
    Ok(v.and_then(|v| serde_json::from_value::<GamePerf>(v).ok()).filter(GamePerf::valid))
}

/// `speed-check` (game → mm, online connection): the waiting room's frame rate.
#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpeedCheck {
    /// The search's mode: `ranked` or `unranked`.
    pub mode: String,
    /// Frames a second over each 300-frame window, in order (1-4).
    pub windows: Vec<f64>,
    /// Gaps of over 1 s between two frames (Dolphin paused, the PC asleep) that started a window
    /// again.
    #[serde(default)]
    pub gaps: u32,
    /// Dolphin's video backend (`GFX.ini` / `Dolphin.ini` GFXBackend).
    #[serde(default)]
    pub backend: String,
}

impl SpeedCheck {
    pub fn parse(v: &serde_json::Value) -> Result<SpeedCheck, String> {
        let c: SpeedCheck = serde_json::from_value(v.clone()).map_err(|e| e.to_string())?;
        if c.mode != "ranked" && c.mode != "unranked" {
            return Err(format!("unknown mode {:?}", c.mode));
        }
        if c.windows.is_empty()
            || c.windows.len() > MAX_CHECK_WINDOWS
            || c.windows.iter().any(|w| !w.is_finite() || !(0.0..=1000.0).contains(w))
        {
            return Err("bad windows".into());
        }
        if c.backend.len() > 32 || c.backend.chars().any(char::is_control) || c.gaps > 10_000 {
            return Err("bad backend or gaps".into());
        }
        Ok(c)
    }

    pub fn verdict(&self) -> Check {
        check_verdict(&self.windows)
    }
}

/// What a waiting-room check says.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Check {
    /// Full speed: no two windows in a row under [`SLOW_FPS`].
    Pass,
    /// Two windows in a row under [`SLOW_FPS`] (one alone may be a shader compiling, as the
    /// match's own check reads it).
    Slow,
    /// Fewer than [`MIN_CHECK_WINDOWS`] windows: the search matched too soon to tell.
    Unsure,
}

pub fn check_verdict(windows: &[f64]) -> Check {
    if windows.len() < MIN_CHECK_WINDOWS {
        return Check::Unsure;
    }
    if windows.windows(2).any(|w| w[0] < SLOW_FPS && w[1] < SLOW_FPS) {
        Check::Slow
    } else {
        Check::Pass
    }
}

/// One thing a player's PC did, oldest first, for [`locked`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    /// A Ranked game, and whether this PC held it back ([`GamePerf::slow_game`]).
    RankedGame { slow: bool },
    /// A waiting-room check (either mode).
    Check(Check),
}

/// The slow Ranked games in a row at the end of `events`: a game at full speed or a passing
/// check ends a run.
pub fn slow_streak(events: &[Event]) -> u32 {
    let mut streak = 0;
    for e in events {
        match e {
            Event::RankedGame { slow: true } => streak += 1,
            Event::RankedGame { slow: false } | Event::Check(Check::Pass) => streak = 0,
            Event::Check(_) => {}
        }
    }
    streak
}

/// Whether the rule would keep this player out of Ranked: [`LOCK_GAMES`] slow Ranked games in a
/// row with no passing check since.
pub fn locked(events: &[Event]) -> bool {
    slow_streak(events) >= LOCK_GAMES
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn perf(frames: u32, fps: f64, own_late: f64) -> GamePerf {
        GamePerf { frames, fps, own_late, followed: 0.0, peer_fps: None, slow: false, peer_slow: false }
    }

    #[test]
    fn a_slow_game_is_this_pcs_own_and_long_enough() {
        assert!(perf(7000, 45.0, 0.9).slow_game());
        // The other player's PC slowed it: few of this PC's frames were late for its own reasons.
        assert!(!perf(7000, 45.0, 0.1).slow_game());
        assert!(!perf(7000, 59.9, 0.9).slow_game());
        assert!(!perf(300, 45.0, 0.9).slow_game());
    }

    #[test]
    fn a_check_needs_two_slow_windows_in_a_row() {
        assert_eq!(check_verdict(&[59.9]), Check::Unsure);
        assert_eq!(check_verdict(&[59.9, 59.8]), Check::Pass);
        // One hitch (a shader compiling) is no slow PC.
        assert_eq!(check_verdict(&[51.0, 59.9, 52.0, 59.9]), Check::Pass);
        assert_eq!(check_verdict(&[59.9, 45.0, 44.0]), Check::Slow);
    }

    #[test]
    fn two_slow_ranked_games_lock_until_a_check_passes() {
        let slow = Event::RankedGame { slow: true };
        let fast = Event::RankedGame { slow: false };
        let (pass, unsure, slow_check) = (Event::Check(Check::Pass), Event::Check(Check::Unsure), Event::Check(Check::Slow));
        assert!(!locked(&[slow]));
        assert!(locked(&[slow, slow]));
        assert!(locked(&[slow, unsure, slow, slow_check]));
        assert!(!locked(&[slow, fast, slow]));
        assert!(!locked(&[slow, slow, pass]));
        // After a pass, two new slow games.
        assert!(!locked(&[slow, slow, pass, slow]));
        assert!(locked(&[slow, slow, pass, slow, slow]));
    }

    #[test]
    fn speed_check_is_checked() {
        let ok = json!({"type": "speed-check", "mode": "ranked", "windows": [59.94, 59.9], "gaps": 0, "backend": "D3D12"});
        assert_eq!(SpeedCheck::parse(&ok).unwrap().verdict(), Check::Pass);
        for bad in [
            json!({"mode": "direct", "windows": [59.9]}),
            json!({"mode": "ranked", "windows": []}),
            json!({"mode": "ranked", "windows": [1.0, 2.0, 3.0, 4.0, 5.0]}),
            json!({"mode": "ranked", "windows": [-1.0]}),
            json!({"mode": "ranked", "windows": [59.9], "backend": "x".repeat(33)}),
        ] {
            assert!(SpeedCheck::parse(&bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn a_bad_perf_is_none() {
        #[derive(Deserialize)]
        struct M {
            #[serde(default, deserialize_with = "lenient_perf")]
            perf: Option<GamePerf>,
        }
        let read = |v: serde_json::Value| serde_json::from_value::<M>(v).unwrap().perf;
        let good = json!({"perf": {"frames": 7000, "fps": 59.9, "ownLate": 0.01, "followed": 0.0, "slow": false, "peerSlow": false}});
        assert_eq!(read(good).map(|p| p.frames), Some(7000));
        assert_eq!(read(json!({})), None);
        assert_eq!(read(json!({"perf": "fast"})), None);
        assert_eq!(read(json!({"perf": {"frames": 7000, "fps": 59.9, "ownLate": 2.0, "followed": 0.0}})), None);
    }
}
