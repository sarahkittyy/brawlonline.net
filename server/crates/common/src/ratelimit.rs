//! A small keyed sliding-window rate limiter.
//!
//! Design section 3: per-IP and per-account limits on login, sign-up and
//! password reset, and `create-ticket` limited per uid. The window sets for
//! accounts are below; `server/README.md` ("Rate limits") explains them. The
//! state is in memory; one process holds it all. Time is passed in so tests
//! are deterministic.

use std::collections::{HashMap, VecDeque};
use std::hash::Hash;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Window {
    pub max: usize,
    pub per: Duration,
}

impl Window {
    pub const fn new(max: usize, per: Duration) -> Self {
        Window { max, per }
    }
}

const MINUTE: Duration = Duration::from_secs(60);
const HOUR: Duration = Duration::from_secs(3600);
pub const DAY: Duration = Duration::from_secs(24 * 3600);

/// The design's default for auth endpoints: 5 per minute and 20 per hour.
/// Login (per IP and per email), password-reset requests and sign-up attempts
/// (per IP), password changes (per account).
pub const AUTH_WINDOWS: [Window; 2] = [Window::new(5, MINUTE), Window::new(20, HOUR)];

/// Accounts created per client IP (an IPv6 /64 counts as one): 3 per hour and
/// 10 per day. Each new account sends one verification email.
pub const SIGNUP_IP_WINDOWS: [Window; 2] = [Window::new(3, HOUR), Window::new(10, DAY)];

/// Emails to one recipient, per kind (verification resends per account,
/// password resets per address): 1 per minute, 3 per hour and 5 per day.
pub const MAIL_RECIPIENT_WINDOWS: [Window; 3] = [Window::new(1, MINUTE), Window::new(3, HOUR), Window::new(5, DAY)];

/// Leaderboard pages per client IP (IPv6 per /64): 30 per minute and 600 per hour. The
/// launcher loads a page of 50 as the player scrolls, so this is 1,500 rows a minute.
pub const LEADERBOARD_IP_WINDOWS: [Window; 2] = [Window::new(30, MINUTE), Window::new(600, HOUR)];

/// Match history pages per account: 60 per minute.
pub const HISTORY_WINDOWS: [Window; 1] = [Window::new(60, MINUTE)];

#[derive(Debug)]
pub struct RateLimiter<K> {
    windows: Vec<Window>,
    hits: HashMap<K, VecDeque<Instant>>,
    longest: Duration,
    last_prune: Option<Instant>,
}

impl<K: Hash + Eq + Clone> RateLimiter<K> {
    pub fn new(windows: &[Window]) -> Self {
        let longest = windows.iter().map(|w| w.per).max().unwrap_or_default();
        RateLimiter { windows: windows.to_vec(), hits: HashMap::new(), longest, last_prune: None }
    }

    /// Records a hit for `key` if allowed. Returns `Err(retry_after)` when any
    /// window is full (the hit is then not recorded).
    pub fn check(&mut self, key: &K, now: Instant) -> Result<(), Duration> {
        self.maybe_prune(now);
        let longest = self.longest;
        let q = self.hits.entry(key.clone()).or_default();
        while q.front().is_some_and(|t| now.saturating_duration_since(*t) >= longest) {
            q.pop_front();
        }
        let mut retry = Duration::ZERO;
        for w in &self.windows {
            let in_window: Vec<&Instant> = q.iter().filter(|t| now.saturating_duration_since(**t) < w.per).collect();
            if in_window.len() >= w.max {
                // The oldest hit in this window decides when a slot frees up.
                let oldest = in_window[in_window.len() - w.max];
                retry = retry.max(w.per - now.saturating_duration_since(*oldest));
            }
        }
        if retry > Duration::ZERO {
            return Err(retry);
        }
        q.push_back(now);
        Ok(())
    }

    /// Forgets a key (for example after a successful login).
    pub fn reset(&mut self, key: &K) {
        self.hits.remove(key);
    }

    fn maybe_prune(&mut self, now: Instant) {
        let due = self.last_prune.is_none_or(|t| now.saturating_duration_since(t) > Duration::from_secs(60));
        if !due {
            return;
        }
        self.last_prune = Some(now);
        let longest = self.longest;
        self.hits.retain(|_, q| q.back().is_some_and(|t| now.saturating_duration_since(*t) < longest));
    }

    pub fn tracked_keys(&self) -> usize {
        self.hits.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn per_minute_and_per_hour() {
        let mut rl = RateLimiter::new(&AUTH_WINDOWS);
        let t0 = Instant::now();
        let k = "1.2.3.4".to_string();
        for i in 0..5 {
            assert!(rl.check(&k, t0 + Duration::from_secs(i)).is_ok());
        }
        let retry = rl.check(&k, t0 + Duration::from_secs(5)).unwrap_err();
        assert_eq!(retry, Duration::from_secs(55));
        // Other keys are independent.
        assert!(rl.check(&"5.6.7.8".to_string(), t0).is_ok());
        // After a minute the minute window frees up; after 20 hits the hour window bites.
        let mut t = t0;
        let mut ok = 5;
        for _ in 0..100 {
            t += Duration::from_secs(61);
            while rl.check(&k, t).is_ok() {
                ok += 1;
            }
            if ok >= 20 {
                break;
            }
        }
        assert_eq!(ok, 20);
        assert!(rl.check(&k, t + Duration::from_secs(61)).is_err());
        assert!(rl.check(&k, t0 + Duration::from_secs(3601 + 61 * 4)).is_ok());
    }

    #[test]
    fn reset_and_prune() {
        let mut rl = RateLimiter::new(&[Window::new(1, Duration::from_secs(2))]);
        let t0 = Instant::now();
        let k = 7u64;
        assert!(rl.check(&k, t0).is_ok());
        assert!(rl.check(&k, t0 + Duration::from_secs(1)).is_err());
        rl.reset(&k);
        assert!(rl.check(&k, t0 + Duration::from_secs(1)).is_ok());
        for i in 0..100u64 {
            let _ = rl.check(&i, t0);
        }
        let _ = rl.check(&1000, t0 + Duration::from_secs(120));
        assert_eq!(rl.tracked_keys(), 1);
    }
}
