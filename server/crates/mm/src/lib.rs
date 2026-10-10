//! The matchmaking server.
//!
//! It speaks Slippi's ENet + JSON ticket protocol (docs/backend-design.md
//! section 1.4) so a client ported from Slippi or Brawlback only needs a new
//! hostname. Direct mode pairs two tickets that name each other's connect codes;
//! Unranked pairs strangers from a FIFO queue (region-aware when a region table
//! is configured). Both clients get each other's external and LAN addresses for
//! hole punching, and the stage list of the mode's ruleset. Rooms (`rooms`) hold
//! 2-4 players who meet by a room code and play from one `get-ticket-resp` that
//! lists all of them. Party gets a clear error.
//!
//! Play keys are checked by reading `users` from Postgres directly. That is
//! simpler than an internal HTTP call to `accounts`: no extra endpoint to
//! secure, one less service in the request path, and mm needs the database
//! anyway to record matches (`mm_matches`) and, later, ratings. Both services
//! share the `common` crate for the key derivation.

pub mod config;
pub mod engine;
pub mod messages;
pub mod region;
pub mod rooms;
pub mod ruleset;
pub mod server;

pub use config::Config;
pub use server::{start, MmHandle};

/// Everything `main` does.
pub fn run(cfg: Config) -> anyhow::Result<()> {
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build()?;
    let secret = common::playkey::PlayKeySecret::parse(&cfg.play_key_secret)?;
    let engine_cfg = cfg.engine_config()?;
    let pool = rt.block_on(
        sqlx::postgres::PgPoolOptions::new()
            .max_connections(5)
            .acquire_timeout(std::time::Duration::from_secs(3))
            .connect(&cfg.database_url),
    )?;
    let handle = start(cfg.listen, cfg.status_addr()?, cfg.max_peers, engine_cfg, secret, pool, rt.handle().clone())?;
    handle.join();
    Ok(())
}
