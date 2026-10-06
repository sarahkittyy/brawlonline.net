//! Helpers for the end-to-end tests in `tests/`.
//!
//! Every test gets its own Postgres database (`e2e_<unix time>_<random>`) in
//! the docker-compose Postgres, and its own in-process `accounts` (HTTP on an
//! ephemeral port, [`MemoryMailer`] so no email leaves the machine) and `mm`
//! (UDP on an ephemeral port).
//!
//! Postgres: `TEST_DATABASE_URL` if set, else the compose service on
//! 127.0.0.1:54329. If it is not reachable, the helpers run
//! `docker compose up -d --wait` in `server/`.

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use accounts::mail::{extract_token, MemoryMailer};
use accounts::store;
use anyhow::Context;
use mm::engine::EngineConfig;
use rand::Rng;
use serde_json::{json, Value};
use sqlx::postgres::PgPoolOptions;
use sqlx::{Executor, PgPool};
use tokio::sync::OnceCell;

pub use accounts::api::UserJson;

pub const DEFAULT_ADMIN_URL: &str = "postgres://pp:pp-dev-password@127.0.0.1:54329/pp";
pub const TEST_SECRET: &str = "5ec7e75ec7e75ec7e75ec7e75ec7e75ec7e75ec7e75ec7e75ec7e75ec7e75ec7";

pub fn server_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn admin_url() -> String {
    std::env::var("TEST_DATABASE_URL").unwrap_or_else(|_| DEFAULT_ADMIN_URL.to_string())
}

async fn try_connect(url: &str) -> Option<PgPool> {
    PgPoolOptions::new().max_connections(2).acquire_timeout(Duration::from_secs(3)).connect(url).await.ok()
}

static ADMIN: OnceCell<String> = OnceCell::const_new();

/// Makes sure Postgres is up and returns the admin connection URL.
pub async fn ensure_postgres() -> anyhow::Result<String> {
    ADMIN
        .get_or_try_init(|| async {
            let url = admin_url();
            if try_connect(&url).await.is_some() {
                return Ok::<_, anyhow::Error>(url);
            }
            anyhow::ensure!(std::env::var("TEST_DATABASE_URL").is_err(), "TEST_DATABASE_URL is set but not reachable");
            let dir = server_dir();
            let status = tokio::task::spawn_blocking(move || {
                std::process::Command::new("docker").args(["compose", "up", "-d", "--wait"]).current_dir(dir).status()
            })
            .await?
            .context("running `docker compose up` (is Docker running?)")?;
            anyhow::ensure!(status.success(), "docker compose up failed");
            for _ in 0..60 {
                if try_connect(&url).await.is_some() {
                    return Ok(url);
                }
                tokio::time::sleep(Duration::from_millis(500)).await;
            }
            anyhow::bail!("Postgres did not come up at {url}")
        })
        .await
        .cloned()
}

fn with_db(url: &str, db: &str) -> String {
    match url.rfind('/') {
        Some(i) => format!("{}/{}", &url[..i], db),
        None => format!("{url}/{db}"),
    }
}

/// A fresh, migrated database. Leftovers from crashed runs older than an hour
/// are dropped on the way.
pub async fn fresh_db() -> anyhow::Result<(PgPool, String)> {
    let admin = ensure_postgres().await?;
    let admin_pool = try_connect(&admin).await.context("admin connection")?;
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
    let old: Vec<String> = sqlx::query_scalar("SELECT datname FROM pg_database WHERE datname LIKE 'e2e\\_%'")
        .fetch_all(&admin_pool)
        .await?;
    for name in old {
        let ts = name.split('_').nth(1).and_then(|t| t.parse::<u64>().ok()).unwrap_or(u64::MAX);
        if ts < now.saturating_sub(3600) {
            let _ = admin_pool.execute(format!("DROP DATABASE IF EXISTS \"{name}\" WITH (FORCE)").as_str()).await;
        }
    }
    let name = format!("e2e_{now}_{:08x}", rand::thread_rng().gen::<u32>());
    admin_pool.execute(format!("CREATE DATABASE \"{name}\"").as_str()).await?;
    admin_pool.close().await;
    let url = with_db(&admin, &name);
    let pool = PgPoolOptions::new().max_connections(10).connect(&url).await?;
    common::db::MIGRATOR.run(&pool).await?;
    Ok((pool, name))
}

pub async fn drop_db(name: &str) {
    if let Ok(admin) = ensure_postgres().await {
        if let Some(p) = try_connect(&admin).await {
            let _ = p.execute(format!("DROP DATABASE IF EXISTS \"{name}\" WITH (FORCE)").as_str()).await;
        }
    }
}

#[derive(Default)]
pub struct StackOptions {
    pub engine: EngineConfig,
    pub rate_limits: bool,
}

/// accounts + mm + a database, all for one test.
pub struct Stack {
    pub base: String,
    pub mm_addr: SocketAddr,
    pub mm: Option<mm::MmHandle>,
    pub pool: PgPool,
    pub db_name: String,
    pub mailer: MemoryMailer,
    pub http: reqwest::Client,
    accounts_task: tokio::task::JoinHandle<()>,
}

impl Stack {
    pub async fn start(opts: StackOptions) -> anyhow::Result<Stack> {
        let (pool, db_name) = fresh_db().await?;
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let mut cfg = accounts::Config::for_tests("unused");
        cfg.play_key_secret = TEST_SECRET.into();
        cfg.public_base_url = format!("http://{addr}");
        cfg.disable_rate_limits = !opts.rate_limits;
        let mailer = MemoryMailer::new();
        let state = accounts::AppState::new(pool.clone(), cfg, Arc::new(mailer.clone()))?;
        let accounts_task = tokio::spawn(async move {
            if let Err(e) = accounts::serve(listener, state).await {
                eprintln!("accounts stopped: {e:#}");
            }
        });
        let secret = common::playkey::PlayKeySecret::parse(TEST_SECRET)?;
        let mm = mm::start(
            "127.0.0.1:0".parse()?,
            64,
            opts.engine,
            secret,
            pool.clone(),
            tokio::runtime::Handle::current(),
        )?;
        Ok(Stack {
            base: format!("http://{addr}"),
            mm_addr: mm.addr,
            mm: Some(mm),
            pool,
            db_name,
            mailer,
            http: reqwest::Client::new(),
            accounts_task,
        })
    }

    pub fn url(&self, path: &str) -> String {
        format!("{}{}", self.base, path)
    }

    pub fn mm_running(&self) -> bool {
        self.mm.as_ref().is_some_and(|m| m.is_running())
    }

    pub async fn invite(&self) -> String {
        store::create_invite(&self.pool, 1, None, Some("test"), "test").await.unwrap().code
    }

    pub async fn post(&self, path: &str, token: Option<&str>, body: Value) -> (u16, Value) {
        let mut req = self.http.post(self.url(path)).json(&body);
        if let Some(t) = token {
            req = req.bearer_auth(t);
        }
        let resp = req.send().await.expect("http");
        let status = resp.status().as_u16();
        let text = resp.text().await.unwrap_or_default();
        (status, serde_json::from_str(&text).unwrap_or(Value::Null))
    }

    pub async fn get(&self, path: &str, token: Option<&str>) -> (u16, Value) {
        let mut req = self.http.get(self.url(path));
        if let Some(t) = token {
            req = req.bearer_auth(t);
        }
        let resp = req.send().await.expect("http");
        let status = resp.status().as_u16();
        let text = resp.text().await.unwrap_or_default();
        (status, serde_json::from_str(&text).unwrap_or(Value::Null))
    }

    /// The token from the last email sent to `email`.
    pub fn mailed_token(&self, email: &str) -> String {
        let mail = self.mailer.last_to(email).unwrap_or_else(|| panic!("no email to {email}"));
        extract_token(&mail.text).expect("token in email")
    }

    /// Sign-up → verify email → pick code prefix → user.json, all over HTTP.
    /// Returns (session token, user.json).
    pub async fn create_player(&self, email: &str, name: &str, prefix: &str) -> (String, UserJson) {
        let invite = self.invite().await;
        let (st, body) = self
            .post(
                "/v1/auth/signup",
                None,
                json!({"email": email, "password": "correct horse battery", "displayName": name, "inviteCode": invite}),
            )
            .await;
        assert_eq!(st, 201, "signup: {body}");
        let session = body["sessionToken"].as_str().unwrap().to_string();
        let token = self.mailed_token(email);
        let (st, body) = self.post("/v1/auth/verify-email", None, json!({"token": token})).await;
        assert_eq!(st, 200, "verify: {body}");
        let (st, body) = self.post("/v1/me/netplay", Some(&session), json!({"codeStart": prefix})).await;
        assert_eq!(st, 200, "netplay: {body}");
        let (st, body) = self.get("/v1/me/user-json", Some(&session)).await;
        assert_eq!(st, 200, "user-json: {body}");
        (session, serde_json::from_value(body).unwrap())
    }

    pub async fn shutdown(mut self) {
        if let Some(m) = self.mm.take() {
            tokio::task::spawn_blocking(move || m.stop()).await.ok();
        }
        self.accounts_task.abort();
        self.pool.close().await;
        drop_db(&self.db_name).await;
    }
}

pub fn creds(u: &UserJson) -> mmclient::Credentials {
    mmclient::Credentials {
        uid: u.uid.clone(),
        play_key: u.play_key.clone(),
        connect_code: Some(u.connect_code.clone()),
        display_name: Some(u.display_name.clone()),
    }
}
