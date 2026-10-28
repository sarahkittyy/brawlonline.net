//! Racial slurs in display names, connect codes and room names: a small LLM on DeepInfra reads
//! each name a player picks (sign-up, rename), each code start (`/v1/me/netplay`) and each room
//! name (`/v1/me/room-name`), and one it calls a
//! racial or ethnic slur is replaced by a kind one ("happy friend", `JOY#123`) before it is
//! stored. Only racial slurs: profanity, sexual words and other insults are left to the admins.
//! With several models (`SLUR_FILTER_MODELS`) every one must say YES. The prompt and the default
//! model were checked against prod's 3715 names and 3039 code starts on 2026-10-26 (README).
//!
//! The check waits for the model (well under a second) but never blocks a sign-up: when DeepInfra
//! fails or is slow the text is stored as typed and checked again in the background
//! ([`recheck_name`], [`recheck_code`]), which replaces it only if the player has not changed it
//! since. Every replacement is in `audit_log` (actor `slur-filter`; `pp-admin user slur-log` lists
//! them). `pp-admin user slur-scan` runs the same check over every account. Off while
//! `DEEPINFRA_API_KEY` is unset.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Duration;

use anyhow::Context;
use rand::seq::SliceRandom;
use serde_json::json;
use uuid::Uuid;

use common::rooms::RoomKind;

use crate::config::SlurFilterConfig;
use crate::store;
use crate::AppState;

/// Display names a slur is replaced by (the launcher's rule: at most 15 characters).
pub const KIND_NAMES: &[&str] = &[
    "love & kindness",
    "happy friend",
    "joyful",
    "whimsy and love",
    "playing toys",
    "kind heart",
    "sunny smiles",
    "gentle soul",
    "hugs for all",
    "cozy blanket",
    "warm cocoa",
    "sharing is fun",
    "lots of love",
    "best buddy",
    "friendship",
];

/// Connect code starts a slur is replaced by.
pub const KIND_PREFIXES: &[&str] = &["JOY", "LOVE", "KIND", "HUG", "HAPY", "FUN", "TOYS", "PAL", "COZY", "NICE"];

/// Who the audit log names for the filter's replacements.
pub const ACTOR: &str = "slur-filter";

/// One answer from the model; waits longer than this count as a failure (stored as typed,
/// checked again later).
const REQUEST_TIMEOUT: Duration = Duration::from_secs(5);

/// Answers kept in memory (a player trying the same name again does not ask again).
const CACHE_MAX: usize = 20_000;

const PROMPT: &str = "You check player names in an online video game for racial slurs. \
Answer YES only if the text clearly is, or clearly spells out, a racial or ethnic slur: \
the n-word in any spelling (nigger, nigga, n1gg3r, niqqa, n i g g a, nibba), chink, kike, spic, wetback, gook, \
raghead and the like, also when hidden inside a longer name (xXniggaXx, superchink). \
A disguised spelling counts when the slur is still plain to read. \
Answer NO when in doubt, and for: ordinary words and names in any language that only look or sound similar \
(Night, Nigel, Niger, Negan, Nipper, Raccoon, Spicy, Chang), profanity, sexual words, \
insults that are not about race or ethnicity, hate groups, symbols and political or historical names, \
and random letters. \
A connect code start is 2 to 4 capital letters: only say YES if those letters are themselves a slur \
(NIG, NIGG, NIGR, NIGA, KIKE, CHNK), not if they merely could stand for one. \
Reply with one word: YES or NO.";

/// What is being checked (the model is told, and answers are cached per kind).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum What {
    DisplayName,
    CodeStart,
    /// The name a host gives their rooms (`PUT /v1/me/room-name`).
    RoomName,
}

impl What {
    fn label(self) -> &'static str {
        match self {
            What::DisplayName => "display name",
            What::CodeStart => "connect code start",
            What::RoomName => "room name",
        }
    }
}

/// The result of a check before the text is stored.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Screened {
    /// Not a slur, or the filter is off.
    Clean,
    /// A slur: store this instead.
    Replace(&'static str),
    /// The model did not answer: store the text and [`recheck_name`] / [`recheck_code`] it.
    Unchecked,
}

struct Remote {
    client: reqwest::Client,
    url: String,
    key: String,
    /// Asked in turn; a slur only when every one says so.
    models: Vec<String>,
}

impl Remote {
    /// One model's answer: YES or NO.
    async fn ask(&self, model: &str, what: What, text: &str) -> anyhow::Result<bool> {
        let body = json!({
            "model": model,
            "messages": [
                {"role": "system", "content": PROMPT},
                {"role": "user", "content": format!("{}: {}", what.label(), serde_json::to_string(text)?)},
            ],
            "temperature": 0,
            "max_tokens": 4,
        });
        let resp = self.client.post(&self.url).bearer_auth(&self.key).json(&body).send().await?;
        let status = resp.status();
        if !status.is_success() {
            let body = resp.text().await.unwrap_or_default();
            anyhow::bail!("deepinfra ({model}) returned {status}: {}", body.chars().take(300).collect::<String>());
        }
        let v: serde_json::Value = resp.json().await?;
        let answer = v["choices"][0]["message"]["content"].as_str().unwrap_or_default();
        let word: String = answer.trim().chars().take_while(|c| c.is_ascii_alphabetic()).collect();
        match word.to_ascii_uppercase().as_str() {
            "YES" => Ok(true),
            "NO" => Ok(false),
            _ => anyhow::bail!("unexpected answer from {model}: {:?}", answer.chars().take(80).collect::<String>()),
        }
    }
}

pub struct SlurFilter {
    remote: Option<Remote>,
    /// Background re-checks after a failed check: waits before each try.
    recheck_waits: Vec<Duration>,
    cache: Mutex<HashMap<(What, String), bool>>,
}

impl SlurFilter {
    pub fn new(cfg: &SlurFilterConfig) -> anyhow::Result<Self> {
        anyhow::ensure!(!cfg.slur_filter_models.is_empty(), "SLUR_FILTER_MODELS is empty");
        let remote = match cfg.deepinfra_api_key.as_ref().filter(|k| !k.is_empty()) {
            None => None,
            Some(key) => Some(Remote {
                client: reqwest::Client::builder().timeout(REQUEST_TIMEOUT).build().context("http client")?,
                url: cfg.slur_filter_url.clone(),
                key: key.expose().to_string(),
                models: cfg.slur_filter_models.iter().map(|m| m.trim().to_string()).filter(|m| !m.is_empty()).collect(),
            }),
        };
        let recheck_waits = cfg.slur_filter_recheck_secs.iter().map(|&s| Duration::from_secs(s)).collect();
        Ok(SlurFilter { remote, recheck_waits, cache: Mutex::new(HashMap::new()) })
    }

    pub fn enabled(&self) -> bool {
        self.remote.is_some()
    }

    /// For the startup log.
    pub fn describe(&self) -> String {
        match &self.remote {
            Some(r) => format!("on ({} at {})", r.models.join(" + "), r.url),
            None => "off (no DEEPINFRA_API_KEY)".into(),
        }
    }

    /// Whether the model calls `text` a racial slur. `Ok(false)` while the filter is off.
    pub async fn is_slur(&self, what: What, text: &str) -> anyhow::Result<bool> {
        let Some(remote) = &self.remote else { return Ok(false) };
        let key = (what, text.trim().to_lowercase());
        if let Some(&hit) = self.cache.lock().unwrap().get(&key) {
            return Ok(hit);
        }
        let mut hit = true;
        for model in &remote.models {
            if !remote.ask(model, what, text).await? {
                hit = false;
                break;
            }
        }
        let mut cache = self.cache.lock().unwrap();
        if cache.len() >= CACHE_MAX {
            cache.clear();
        }
        cache.insert(key, hit);
        Ok(hit)
    }

    /// The check before storing `text`: what to store, or [`Screened::Unchecked`] on a failure.
    pub async fn screen(&self, what: What, text: &str) -> Screened {
        match self.is_slur(what, text).await {
            Ok(false) => Screened::Clean,
            Ok(true) => Screened::Replace(match what {
                What::DisplayName | What::RoomName => kind_name(),
                What::CodeStart => kind_prefix(),
            }),
            Err(e) => {
                tracing::warn!("slur check of {} failed: {e:#}", what.label());
                Screened::Unchecked
            }
        }
    }
}

pub fn kind_name() -> &'static str {
    KIND_NAMES.choose(&mut rand::thread_rng()).unwrap()
}

pub fn kind_prefix() -> &'static str {
    KIND_PREFIXES.choose(&mut rand::thread_rng()).unwrap()
}

/// The display name to store for `name` (a kind one if it is a slur), and whether it still has
/// to be checked ([`recheck_name`] once it is stored).
pub async fn screen_name(state: &AppState, name: &str) -> (String, bool) {
    match state.slur_filter.screen(What::DisplayName, name).await {
        Screened::Clean => (name.to_string(), false),
        Screened::Replace(kind) => (kind.to_string(), false),
        Screened::Unchecked => (name.to_string(), true),
    }
}

/// The code start to assign for `prefix` (validated, uppercase), and whether the code still
/// has to be checked ([`recheck_code`] once it is assigned).
pub async fn screen_code_start(state: &AppState, prefix: &str) -> (String, bool) {
    match state.slur_filter.screen(What::CodeStart, prefix).await {
        Screened::Clean => (prefix.to_string(), false),
        Screened::Replace(kind) => (kind.to_string(), false),
        Screened::Unchecked => (prefix.to_string(), true),
    }
}

/// The room name to store for `name` (a kind one if it is a slur), and whether it still has to
/// be checked ([`recheck_room_name`] once it is stored).
pub async fn screen_room_name(state: &AppState, name: &str) -> (String, bool) {
    match state.slur_filter.screen(What::RoomName, name).await {
        Screened::Clean => (name.to_string(), false),
        Screened::Replace(kind) => (kind.to_string(), false),
        Screened::Unchecked => (name.to_string(), true),
    }
}

/// Logs a replacement made before the text was stored ([`screen_name`] and the like gave a kind
/// text for `from`), once the account has it: the journal and `audit_log` as the late ones, so
/// `pp-admin user slur-log` lists every replacement. A failed audit only warns.
pub async fn audit_screened(state: &AppState, action: &str, uid: Uuid, from: &str, to: &str) {
    tracing::info!(%uid, from, to, action, "racial slur replaced");
    if let Err(e) = audit_replacement(&state.pool, ACTOR, action, uid, from, to).await {
        tracing::warn!(%uid, "audit of a slur replacement failed: {e}");
    }
}

/// Logs a replacement made after the text was stored (the background re-check, `slur-scan`).
pub async fn audit_replacement(
    pool: &sqlx::PgPool,
    actor: &str,
    action: &str,
    uid: Uuid,
    from: &str,
    to: &str,
) -> sqlx::Result<()> {
    store::audit(pool, actor, action, Some(&uid.to_string()), json!({"from": from, "to": to, "reason": "racial slur"}))
        .await
}

/// Checks a display name stored unchecked again in the background until the model answers, and
/// replaces it if it is a slur and still the account's name.
pub fn recheck_name(state: &AppState, uid: Uuid, name: String) {
    let state = state.clone();
    tokio::spawn(async move {
        for wait in state.slur_filter.recheck_waits.clone() {
            tokio::time::sleep(wait).await;
            match state.slur_filter.is_slur(What::DisplayName, &name).await {
                Ok(false) => return,
                Ok(true) => {
                    let kind = kind_name();
                    match store::rename_if(&state.pool, uid, &name, kind).await {
                        Ok(true) => {
                            tracing::info!(%uid, from = %name, to = kind, "racial slur in a display name replaced");
                            if let Err(e) = audit_replacement(&state.pool, ACTOR, "user.rename", uid, &name, kind).await
                            {
                                tracing::warn!("audit of a slur rename failed: {e}");
                            }
                        }
                        Ok(false) => {}
                        Err(e) => tracing::warn!(%uid, "slur rename failed: {e}"),
                    }
                    return;
                }
                Err(e) => tracing::warn!(%uid, "slur re-check of a display name failed: {e:#}"),
            }
        }
        tracing::warn!(%uid, "display name left unchecked: the slur filter did not answer");
    });
}

/// [`recheck_name`] for a room name stored unchecked: replaced if it is a slur and still the
/// account's name for that kind of room (mm renames the room it hosts).
pub fn recheck_room_name(state: &AppState, uid: Uuid, kind: RoomKind, name: String) {
    let state = state.clone();
    tokio::spawn(async move {
        for wait in state.slur_filter.recheck_waits.clone() {
            tokio::time::sleep(wait).await;
            match state.slur_filter.is_slur(What::RoomName, &name).await {
                Ok(false) => return,
                Ok(true) => {
                    let kind_text = kind_name();
                    match store::set_room_name_if(&state.pool, uid, kind, &name, kind_text).await {
                        Ok(true) => {
                            tracing::info!(%uid, from = %name, to = kind_text, "racial slur in a room name replaced");
                            if let Err(e) =
                                audit_replacement(&state.pool, ACTOR, "user.room-name", uid, &name, kind_text).await
                            {
                                tracing::warn!("audit of a slur room name failed: {e}");
                            }
                        }
                        Ok(false) => {}
                        Err(e) => tracing::warn!(%uid, "slur room name replacement failed: {e}"),
                    }
                    return;
                }
                Err(e) => tracing::warn!(%uid, "slur re-check of a room name failed: {e:#}"),
            }
        }
        tracing::warn!(%uid, "room name left unchecked: the slur filter did not answer");
    });
}

/// [`recheck_name`] for a connect code assigned unchecked: a slur start gets a kind start (a
/// new number with it), if the account still has `code`.
pub fn recheck_code(state: &AppState, uid: Uuid, code: String) {
    let state = state.clone();
    tokio::spawn(async move {
        let prefix = code.split_once('#').map(|(p, _)| p.to_string()).unwrap_or_else(|| code.clone());
        for wait in state.slur_filter.recheck_waits.clone() {
            tokio::time::sleep(wait).await;
            match state.slur_filter.is_slur(What::CodeStart, &prefix).await {
                Ok(false) => return,
                Ok(true) => {
                    match store::reassign_connect_code(&state.pool, uid, &code, kind_prefix()).await {
                        Ok(new) => {
                            tracing::info!(%uid, from = %code, to = %new, "racial slur in a connect code replaced");
                            if let Err(e) =
                                audit_replacement(&state.pool, ACTOR, "user.set_code", uid, &code, &new).await
                            {
                                tracing::warn!("audit of a slur code change failed: {e}");
                            }
                        }
                        Err(store::AssignCodeError::AlreadyAssigned) => {}
                        Err(e) => tracing::warn!(%uid, "slur code change failed: {e}"),
                    }
                    return;
                }
                Err(e) => tracing::warn!(%uid, "slur re-check of a connect code failed: {e:#}"),
            }
        }
        tracing::warn!(%uid, "connect code left unchecked: the slur filter did not answer");
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Secret;
    use axum::{extract::State, routing::post, Json, Router};
    use std::sync::Arc;

    /// A local stand-in for DeepInfra: answers YES for texts containing "bad", "NO" otherwise
    /// (model `test/strict`: always NO), and records the requests.
    async fn fake(answer_status: axum::http::StatusCode) -> (SlurFilter, Arc<Mutex<Vec<serde_json::Value>>>) {
        type Seen = Arc<Mutex<Vec<serde_json::Value>>>;
        let seen: Seen = Default::default();
        let app = Router::new()
            .route(
                "/chat",
                post(move |State(seen): State<Seen>, Json(body): Json<serde_json::Value>| async move {
                    let text = body["messages"][1]["content"].as_str().unwrap_or_default().to_string();
                    let strict = body["model"] == "test/strict";
                    seen.lock().unwrap().push(body);
                    let word = if (text.contains("bad") || text.contains("BAD")) && !strict { " YES." } else { "NO" };
                    (answer_status, Json(json!({"choices": [{"message": {"role": "assistant", "content": word}}]})))
                }),
            )
            .with_state(seen.clone());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
        let cfg = SlurFilterConfig {
            deepinfra_api_key: Some(Secret::new("di_test")),
            slur_filter_models: vec!["test/model".into()],
            slur_filter_url: format!("http://{addr}/chat"),
            ..SlurFilterConfig::default()
        };
        (SlurFilter::new(&cfg).unwrap(), seen)
    }

    #[tokio::test]
    async fn answers_are_read_and_cached() {
        let (f, seen) = fake(axum::http::StatusCode::OK).await;
        assert!(f.is_slur(What::DisplayName, "so bad").await.unwrap());
        assert!(!f.is_slur(What::DisplayName, "Night").await.unwrap());
        assert!(f.is_slur(What::DisplayName, "SO BAD ").await.unwrap(), "cached case-insensitively");
        assert!(f.is_slur(What::CodeStart, "BAD").await.unwrap());
        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 3);
        assert_eq!(seen[0]["model"], "test/model");
        assert_eq!(seen[0]["messages"][1]["content"], "display name: \"so bad\"");
        assert_eq!(seen[2]["messages"][1]["content"], "connect code start: \"BAD\"");
    }

    #[tokio::test]
    async fn screen_replaces_slurs_and_fails_open() {
        let (f, _) = fake(axum::http::StatusCode::OK).await;
        assert_eq!(f.screen(What::DisplayName, "Night").await, Screened::Clean);
        assert!(matches!(f.screen(What::DisplayName, "bad").await, Screened::Replace(n) if KIND_NAMES.contains(&n)));
        assert!(matches!(f.screen(What::CodeStart, "BAD").await, Screened::Replace(p) if KIND_PREFIXES.contains(&p)));
        let (down, _) = fake(axum::http::StatusCode::SERVICE_UNAVAILABLE).await;
        assert_eq!(down.screen(What::DisplayName, "bad").await, Screened::Unchecked);
        let off = SlurFilter::new(&SlurFilterConfig::default()).unwrap();
        assert!(!off.enabled());
        assert_eq!(off.screen(What::DisplayName, "bad").await, Screened::Clean);
    }

    #[tokio::test]
    async fn every_model_must_agree() {
        let (mut f, seen) = fake(axum::http::StatusCode::OK).await;
        f.remote.as_mut().unwrap().models = vec!["test/model".into(), "test/strict".into()];
        assert!(!f.is_slur(What::DisplayName, "bad").await.unwrap());
        assert!(!f.is_slur(What::DisplayName, "Night").await.unwrap());
        let models: Vec<_> = seen.lock().unwrap().iter().map(|b| b["model"].as_str().unwrap().to_string()).collect();
        assert_eq!(models, ["test/model", "test/strict", "test/model"], "a NO ends the round");
    }

    #[test]
    fn kind_names_and_prefixes_are_valid() {
        for n in KIND_NAMES {
            common::codes::validate_display_name(n).unwrap();
        }
        for p in KIND_PREFIXES {
            assert_eq!(common::codes::validate_code_start(p).unwrap(), *p);
        }
    }
}
