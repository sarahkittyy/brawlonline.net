use std::net::SocketAddr;
use std::time::Duration;

use clap::Parser;

use crate::engine::EngineConfig;
use crate::ruleset::Rulesets;

#[derive(Debug, Clone, Parser)]
#[command(name = "mm", about = "Matchmaking server (Slippi ENet + JSON ticket protocol)", version)]
pub struct Config {
    /// Postgres connection string. mm only reads `users` and inserts into `mm_matches`.
    #[arg(long, env = "DATABASE_URL")]
    pub database_url: String,

    /// UDP address to listen on. The Slippi client uses port 43113.
    #[arg(long, env = "MM_LISTEN", default_value = "0.0.0.0:43113")]
    pub listen: SocketAddr,

    /// Same secret as the accounts service (play keys are derived from it).
    #[arg(long, env = "PLAY_KEY_SECRET", hide_env_values = true)]
    pub play_key_secret: String,

    /// Seconds before a waiting ticket ends with an explicit error.
    #[arg(long, env = "MM_TICKET_TTL_SECS", default_value_t = 600)]
    pub ticket_ttl_secs: u64,

    /// Refuse clients whose `appVersion` is below this.
    #[arg(long, env = "MM_MIN_APP_VERSION")]
    pub min_app_version: Option<String>,

    /// Version named in "out of date" errors.
    #[arg(long, env = "LATEST_VERSION")]
    pub latest_version: Option<String>,

    /// Rulesets JSON (stage lists per mode). Defaults to the built-in config/rulesets.json.
    #[arg(long, env = "MM_RULESETS_FILE")]
    pub rulesets_file: Option<String>,

    /// Maximum simultaneous ENet peers.
    #[arg(long, env = "MM_MAX_PEERS", default_value_t = 4000)]
    pub max_peers: usize,

    /// Seconds between tickets from one account.
    #[arg(long, env = "MM_TICKET_INTERVAL_SECS", default_value_t = 2)]
    pub ticket_interval_secs: u64,
}

impl Config {
    pub fn engine_config(&self) -> anyhow::Result<EngineConfig> {
        Ok(EngineConfig {
            ticket_ttl: Duration::from_secs(self.ticket_ttl_secs),
            ticket_interval: Duration::from_secs(self.ticket_interval_secs),
            min_app_version: self.min_app_version.clone().filter(|s| !s.is_empty()),
            latest_version: self.latest_version.clone().filter(|s| !s.is_empty()),
            rulesets: Rulesets::load(self.rulesets_file.as_deref())?,
            ..EngineConfig::default()
        })
    }

    pub fn for_tests(database_url: &str, play_key_secret: &str) -> Self {
        Config {
            database_url: database_url.into(),
            listen: "127.0.0.1:0".parse().unwrap(),
            play_key_secret: play_key_secret.into(),
            ticket_ttl_secs: 600,
            min_app_version: None,
            latest_version: None,
            rulesets_file: None,
            max_peers: 64,
            ticket_interval_secs: 2,
        }
    }
}
