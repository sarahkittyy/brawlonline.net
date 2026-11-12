//! The name filter: slurs in display names, connect codes, room names and social media usernames.
//! Each name a player picks (sign-up, rename), each code start (`/v1/me/netplay`), each room name
//! (`/v1/me/room-name`) and each social media username (`/v1/me/profile`) is checked against the
//! rules in `name_filter_rules`, and one they flag is replaced by a kind one ("happy friend",
//! `JOY#123`) before it is stored (a social media username is refused instead).
//!
//! The rules live in the database, not here, so the prompt and the words stay out of the public
//! repository (Sarah's board, 2026-11-12); `pp-admin filter` adds, edits, tests and turns them on.
//! A text is flagged when no `allow` rule matches it and either a `word` or `regex` rule matches it
//! (no model needed) or, for an `ai` rule, every model in `SLUR_FILTER_MODELS` answers YES to its
//! prompt. accounts reads the rules again every [`RULES_TTL`]. The model's answers are kept per
//! prompt in `name_filter_answers`, so `pp-admin filter test` and `user slur-scan` only pay for
//! texts that prompt has not seen.
//!
//! The check waits for the model (well under a second) but never blocks a sign-up: when DeepInfra
//! fails or is slow the text is stored as typed and checked again in the background
//! ([`recheck_name`], [`recheck_code`]), which replaces it only if the player has not changed it
//! since. Every replacement is in `audit_log` (actor `slur-filter`; `pp-admin user slur-log` lists
//! them). `ai` rules are skipped while `DEEPINFRA_API_KEY` is unset.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::Context;
use rand::seq::SliceRandom;
use serde_json::json;
use sha2::{Digest, Sha256};
use sqlx::PgPool;
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

/// The audit log's `reason` for a replacement ("racial slur" until 2026-10-31).
pub const REASON: &str = "slur";

/// One answer from the model; waits longer than this count as a failure (stored as typed,
/// checked again later).
const REQUEST_TIMEOUT: Duration = Duration::from_secs(5);

/// Answers kept in memory (a player trying the same name again does not ask again).
const CACHE_MAX: usize = 20_000;

/// How long accounts uses the rules it read before reading them again (a `pp-admin filter`
/// change is live within this).
pub const RULES_TTL: Duration = Duration::from_secs(30);

/// What is being checked (the model is told, and answers are kept per kind).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum What {
    DisplayName,
    CodeStart,
    /// The name a host gives their rooms (`PUT /v1/me/room-name`).
    RoomName,
    /// A social media username on a profile (`PUT /v1/me/profile`): refused, not replaced.
    SocialHandle,
}

impl What {
    pub const ALL: [What; 4] = [What::DisplayName, What::CodeStart, What::RoomName, What::SocialHandle];

    /// What the model is told it is reading.
    pub fn label(self) -> &'static str {
        match self {
            What::DisplayName => "display name",
            What::CodeStart => "connect code start",
            What::RoomName => "room name",
            What::SocialHandle => "social media username",
        }
    }

    /// The database's word for it (`name_filter_answers.what`, `name_filter_samples.what`).
    pub fn key(self) -> &'static str {
        match self {
            What::DisplayName => "name",
            What::CodeStart => "code",
            What::RoomName => "room",
            What::SocialHandle => "social",
        }
    }

    pub fn parse(key: &str) -> Option<What> {
        What::ALL.into_iter().find(|w| w.key() == key)
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

/// A rule's kind (`name_filter_rules.kind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RuleKind {
    /// The model's system prompt: flagged when every model answers YES.
    Ai,
    /// Found anywhere in the [`fold`]ed text (the word is folded too).
    Word,
    /// A case-insensitive regular expression, tried on the text as typed and folded.
    Regex,
    /// A text (any case, spaces around ignored) never flagged.
    Allow,
}

impl RuleKind {
    pub const ALL: [RuleKind; 4] = [RuleKind::Ai, RuleKind::Word, RuleKind::Regex, RuleKind::Allow];

    pub fn as_str(self) -> &'static str {
        match self {
            RuleKind::Ai => "ai",
            RuleKind::Word => "word",
            RuleKind::Regex => "regex",
            RuleKind::Allow => "allow",
        }
    }

    pub fn parse(s: &str) -> Option<RuleKind> {
        RuleKind::ALL.into_iter().find(|k| k.as_str() == s)
    }
}

/// A row of `name_filter_rules`.
#[derive(Debug, Clone, sqlx::FromRow, serde::Serialize, serde::Deserialize)]
pub struct Rule {
    #[serde(default)]
    pub id: i64,
    pub kind: String,
    pub body: String,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub enabled: bool,
    #[serde(default)]
    pub created_by: String,
    #[serde(default = "chrono::Utc::now")]
    pub created_at: chrono::DateTime<chrono::Utc>,
    #[serde(default = "chrono::Utc::now")]
    pub updated_at: chrono::DateTime<chrono::Utc>,
}

/// A row of `name_filter_samples`: a text `pp-admin filter test` expects flagged or passed.
#[derive(Debug, Clone, sqlx::FromRow, serde::Serialize, serde::Deserialize)]
pub struct Sample {
    #[serde(default)]
    pub id: i64,
    pub what: String,
    pub text: String,
    pub expect_flag: bool,
    #[serde(default)]
    pub note: String,
}

/// The text a `word` rule looks in: lowercase, digits and symbols that stand in for letters read
/// as those letters (`1` and `!` as i, `3` as e, `@` and `4` as a, `$` and `5` as s, ...), and
/// everything that is not a letter left out, so spaces, dots and underscores between the letters
/// do not hide a word.
pub fn fold(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars().flat_map(char::to_lowercase) {
        let c = match c {
            '0' => 'o',
            '1' | '!' | '|' => 'i',
            '3' => 'e',
            '4' | '@' => 'a',
            '5' | '$' => 's',
            '7' | '+' => 't',
            '8' => 'b',
            '9' => 'g',
            c => c,
        };
        if c.is_alphabetic() {
            out.push(c);
        }
    }
    out
}

/// How texts are compared for `allow` rules and kept in the answer cache.
pub fn answer_key(text: &str) -> String {
    text.trim().to_lowercase()
}

/// The answer cache's key for a prompt: the models asked and the prompt, hashed.
pub fn prompt_hash(models: &[String], prompt: &str) -> String {
    let mut h = Sha256::new();
    h.update(models.join("\n").as_bytes());
    h.update([0]);
    h.update(prompt.as_bytes());
    hex::encode(&h.finalize()[..16])
}

/// Whether `body` makes a rule of `kind` (a regex compiles, an `ai` prompt is not blank).
pub fn check_rule(kind: RuleKind, body: &str) -> anyhow::Result<()> {
    anyhow::ensure!(!body.trim().is_empty(), "the rule is empty");
    match kind {
        RuleKind::Regex => {
            build_regex(body)?;
        }
        RuleKind::Word => anyhow::ensure!(!fold(body).is_empty(), "{body:?} has no letters once folded"),
        RuleKind::Ai | RuleKind::Allow => {}
    }
    Ok(())
}

fn build_regex(body: &str) -> anyhow::Result<regex::Regex> {
    regex::RegexBuilder::new(body)
        .case_insensitive(true)
        .size_limit(1 << 20)
        .build()
        .with_context(|| format!("bad regex {body:?}"))
}

/// An `ai` rule ready to ask.
#[derive(Debug, Clone)]
pub struct Prompt {
    pub rule: i64,
    pub text: String,
    pub hash: String,
}

/// What the rules without a model say about a text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Local {
    /// An `allow` rule: never flagged.
    Allowed(i64),
    /// A `word` or `regex` rule flags it.
    Flagged(i64),
    /// Up to the `ai` rules.
    AskModel,
}

/// A set of rules ready to check texts with (the enabled ones, or a set `filter test` tries).
#[derive(Debug, Default)]
pub struct RuleSet {
    allow: Vec<(i64, String)>,
    words: Vec<(i64, String)>,
    regexes: Vec<(i64, regex::Regex)>,
    pub prompts: Vec<Prompt>,
}

impl RuleSet {
    /// Every rule given, enabled or not (the caller picks). A rule that does not build (a bad
    /// regex written to the table by hand) is left out with a warning.
    pub fn compile<'a>(rules: impl IntoIterator<Item = &'a Rule>, models: &[String]) -> RuleSet {
        let mut set = RuleSet::default();
        for r in rules {
            match RuleKind::parse(&r.kind) {
                Some(RuleKind::Allow) => set.allow.push((r.id, answer_key(&r.body))),
                Some(RuleKind::Word) => set.words.push((r.id, fold(&r.body))),
                Some(RuleKind::Regex) => match build_regex(&r.body) {
                    Ok(re) => set.regexes.push((r.id, re)),
                    Err(e) => tracing::warn!("name filter rule #{} left out: {e:#}", r.id),
                },
                Some(RuleKind::Ai) => {
                    set.prompts.push(Prompt { rule: r.id, text: r.body.clone(), hash: prompt_hash(models, &r.body) })
                }
                None => tracing::warn!("name filter rule #{} has an unknown kind {:?}", r.id, r.kind),
            }
        }
        set
    }

    pub fn is_empty(&self) -> bool {
        self.allow.is_empty() && self.words.is_empty() && self.regexes.is_empty() && self.prompts.is_empty()
    }

    /// The `allow`, `word` and `regex` rules' say.
    pub fn local(&self, text: &str) -> Local {
        let key = answer_key(text);
        if let Some((id, _)) = self.allow.iter().find(|(_, a)| *a == key) {
            return Local::Allowed(*id);
        }
        let folded = fold(text);
        if let Some((id, _)) = self.words.iter().find(|(_, w)| !w.is_empty() && folded.contains(w.as_str())) {
            return Local::Flagged(*id);
        }
        if let Some((id, _)) = self.regexes.iter().find(|(_, re)| re.is_match(text) || re.is_match(&folded)) {
            return Local::Flagged(*id);
        }
        Local::AskModel
    }
}

struct Remote {
    client: reqwest::Client,
    url: String,
    key: String,
}

impl Remote {
    /// One model's answer to `prompt`: YES or NO.
    async fn ask(&self, model: &str, prompt: &str, what: What, text: &str) -> anyhow::Result<bool> {
        let body = json!({
            "model": model,
            "messages": [
                {"role": "system", "content": prompt},
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

/// Where the rules come from.
enum Source {
    /// `name_filter_rules` (enabled ones), read again every [`RULES_TTL`]; answers kept in
    /// `name_filter_answers` too.
    Db(PgPool),
    /// A fixed set (tests).
    Fixed(Arc<RuleSet>),
}

type AnswerKey = (String, What, String);

pub struct SlurFilter {
    remote: Option<Remote>,
    /// Asked in turn; YES only when every one says so.
    models: Vec<String>,
    source: Source,
    loaded: tokio::sync::Mutex<Option<(Instant, Arc<RuleSet>)>>,
    /// Background re-checks after a failed check: waits before each try.
    recheck_waits: Vec<Duration>,
    cache: Mutex<HashMap<AnswerKey, bool>>,
}

impl SlurFilter {
    /// The filter with the database's rules.
    pub fn new(cfg: &SlurFilterConfig, pool: PgPool) -> anyhow::Result<Self> {
        Self::build(cfg, Source::Db(pool))
    }

    /// The filter with fixed rules and no database (tests).
    pub fn with_rules(cfg: &SlurFilterConfig, rules: &[Rule]) -> anyhow::Result<Self> {
        let models = models(cfg);
        Self::build(cfg, Source::Fixed(Arc::new(RuleSet::compile(rules, &models))))
    }

    fn build(cfg: &SlurFilterConfig, source: Source) -> anyhow::Result<Self> {
        let models = models(cfg);
        anyhow::ensure!(!models.is_empty(), "SLUR_FILTER_MODELS is empty");
        let remote = match cfg.deepinfra_api_key.as_ref().filter(|k| !k.is_empty()) {
            None => None,
            Some(key) => Some(Remote {
                client: reqwest::Client::builder().timeout(REQUEST_TIMEOUT).build().context("http client")?,
                url: cfg.slur_filter_url.clone(),
                key: key.expose().to_string(),
            }),
        };
        let recheck_waits = cfg.slur_filter_recheck_secs.iter().map(|&s| Duration::from_secs(s)).collect();
        Ok(SlurFilter {
            remote,
            models,
            source,
            loaded: tokio::sync::Mutex::new(None),
            recheck_waits,
            cache: Mutex::new(HashMap::new()),
        })
    }

    /// Whether the model can be asked (`ai` rules are skipped without it).
    pub fn enabled(&self) -> bool {
        self.remote.is_some()
    }

    pub fn models(&self) -> &[String] {
        &self.models
    }

    /// For the startup log.
    pub fn describe(&self) -> String {
        match &self.remote {
            Some(r) => format!("on: rules from name_filter_rules, model {} at {}", self.models.join(" + "), r.url),
            None => "on: rules from name_filter_rules without ai ones (no DEEPINFRA_API_KEY)".into(),
        }
    }

    /// The rules in use: the enabled ones, read again once [`RULES_TTL`] has passed. When the
    /// database fails the last ones read stay in use.
    pub async fn rules(&self) -> anyhow::Result<Arc<RuleSet>> {
        let pool = match &self.source {
            Source::Fixed(set) => return Ok(set.clone()),
            Source::Db(pool) => pool,
        };
        let mut loaded = self.loaded.lock().await;
        if let Some((at, set)) = &*loaded {
            if at.elapsed() < RULES_TTL {
                return Ok(set.clone());
            }
        }
        match load_rules(pool, true).await {
            Ok(rules) => {
                let set = Arc::new(RuleSet::compile(&rules, &self.models));
                *loaded = Some((Instant::now(), set.clone()));
                Ok(set)
            }
            Err(e) => match &mut *loaded {
                Some((at, set)) => {
                    tracing::warn!("name filter rules not read again: {e}");
                    *at = Instant::now();
                    Ok(set.clone())
                }
                None => Err(anyhow::Error::from(e).context("reading the name filter rules")),
            },
        }
    }

    /// An answer already known for `prompt` (memory, then `name_filter_answers`), without asking.
    pub async fn known_answer(&self, prompt: &Prompt, what: What, text: &str) -> Option<bool> {
        let key = (prompt.hash.clone(), what, answer_key(text));
        if let Some(&hit) = self.cache.lock().unwrap().get(&key) {
            return Some(hit);
        }
        let Source::Db(pool) = &self.source else { return None };
        match sqlx::query_scalar::<_, bool>(
            "SELECT flagged FROM name_filter_answers WHERE prompt_hash = $1 AND what = $2 AND text = $3",
        )
        .bind(&key.0)
        .bind(what.key())
        .bind(&key.2)
        .fetch_optional(pool)
        .await
        {
            Ok(Some(hit)) => {
                self.remember(key, hit);
                Some(hit)
            }
            Ok(None) => None,
            Err(e) => {
                tracing::warn!("reading a name filter answer failed: {e}");
                None
            }
        }
    }

    /// Whether every model answers YES to `prompt` about `text` (a known answer when there is one).
    pub async fn ask(&self, prompt: &Prompt, what: What, text: &str) -> anyhow::Result<bool> {
        if let Some(hit) = self.known_answer(prompt, what, text).await {
            return Ok(hit);
        }
        let remote = self.remote.as_ref().context("no DEEPINFRA_API_KEY: the model cannot be asked")?;
        let mut hit = true;
        for model in &self.models {
            if !remote.ask(model, &prompt.text, what, text).await? {
                hit = false;
                break;
            }
        }
        let key = (prompt.hash.clone(), what, answer_key(text));
        if let Source::Db(pool) = &self.source {
            let stored = sqlx::query(
                "INSERT INTO name_filter_answers (prompt_hash, what, text, flagged) VALUES ($1, $2, $3, $4)
                 ON CONFLICT (prompt_hash, what, text) DO UPDATE SET flagged = EXCLUDED.flagged, asked_at = now()",
            )
            .bind(&key.0)
            .bind(what.key())
            .bind(&key.2)
            .bind(hit)
            .execute(pool)
            .await;
            if let Err(e) = stored {
                tracing::warn!("keeping a name filter answer failed: {e}");
            }
        }
        self.remember(key, hit);
        Ok(hit)
    }

    fn remember(&self, key: AnswerKey, hit: bool) {
        let mut cache = self.cache.lock().unwrap();
        if cache.len() >= CACHE_MAX {
            cache.clear();
        }
        cache.insert(key, hit);
    }

    /// The rule in `set` that flags `text`, if any. `ai` rules are asked in order (skipped
    /// without a key); an error means the model did not answer.
    pub async fn check(&self, set: &RuleSet, what: What, text: &str) -> anyhow::Result<Option<i64>> {
        match set.local(text) {
            Local::Allowed(_) => Ok(None),
            Local::Flagged(id) => Ok(Some(id)),
            Local::AskModel => {
                if self.remote.is_none() {
                    return Ok(None);
                }
                for p in &set.prompts {
                    if self.ask(p, what, text).await? {
                        return Ok(Some(p.rule));
                    }
                }
                Ok(None)
            }
        }
    }

    /// Whether the rules in use flag `text`.
    pub async fn is_slur(&self, what: What, text: &str) -> anyhow::Result<bool> {
        let set = self.rules().await?;
        Ok(self.check(&set, what, text).await?.is_some())
    }

    /// The check before storing `text`: what to store, or [`Screened::Unchecked`] on a failure.
    pub async fn screen(&self, what: What, text: &str) -> Screened {
        match self.is_slur(what, text).await {
            Ok(false) => Screened::Clean,
            Ok(true) => Screened::Replace(match what {
                What::DisplayName | What::RoomName | What::SocialHandle => kind_name(),
                What::CodeStart => kind_prefix(),
            }),
            Err(e) => {
                tracing::warn!("slur check of {} failed: {e:#}", what.label());
                Screened::Unchecked
            }
        }
    }
}

fn models(cfg: &SlurFilterConfig) -> Vec<String> {
    cfg.slur_filter_models.iter().map(|m| m.trim().to_string()).filter(|m| !m.is_empty()).collect()
}

const RULE_COLS: &str = "id, kind, body, note, enabled, created_by, created_at, updated_at";

/// The rules, oldest first (only the enabled ones with `only_enabled`).
pub async fn load_rules(pool: &PgPool, only_enabled: bool) -> sqlx::Result<Vec<Rule>> {
    sqlx::query_as(&format!(
        "SELECT {RULE_COLS} FROM name_filter_rules {} ORDER BY id",
        if only_enabled { "WHERE enabled" } else { "" }
    ))
    .fetch_all(pool)
    .await
}

pub async fn get_rule(pool: &PgPool, id: i64) -> sqlx::Result<Option<Rule>> {
    sqlx::query_as(&format!("SELECT {RULE_COLS} FROM name_filter_rules WHERE id = $1"))
        .bind(id)
        .fetch_optional(pool)
        .await
}

/// Adds a rule (checked with [`check_rule`]); its id.
pub async fn add_rule(
    pool: &PgPool,
    kind: RuleKind,
    body: &str,
    note: &str,
    enabled: bool,
    by: &str,
) -> anyhow::Result<i64> {
    check_rule(kind, body)?;
    Ok(sqlx::query_scalar(
        "INSERT INTO name_filter_rules (kind, body, note, enabled, created_by) VALUES ($1, $2, $3, $4, $5) RETURNING id",
    )
    .bind(kind.as_str())
    .bind(body)
    .bind(note)
    .bind(enabled)
    .bind(by)
    .fetch_one(pool)
    .await?)
}

/// The texts `filter test` expects flagged or passed, oldest first.
pub async fn load_samples(pool: &PgPool) -> sqlx::Result<Vec<Sample>> {
    sqlx::query_as("SELECT id, what, text, expect_flag, note FROM name_filter_samples ORDER BY id")
        .fetch_all(pool)
        .await
}

/// Adds a sample, or changes what it expects if it is there; its id.
pub async fn add_sample(
    pool: &PgPool,
    what: What,
    text: &str,
    expect_flag: bool,
    note: &str,
    by: &str,
) -> anyhow::Result<i64> {
    anyhow::ensure!(!text.trim().is_empty(), "the sample is empty");
    Ok(sqlx::query_scalar(
        "INSERT INTO name_filter_samples (what, text, expect_flag, note, created_by) VALUES ($1, $2, $3, $4, $5)
         ON CONFLICT (what, text) DO UPDATE SET expect_flag = EXCLUDED.expect_flag, note = EXCLUDED.note
         RETURNING id",
    )
    .bind(what.key())
    .bind(text)
    .bind(expect_flag)
    .bind(note)
    .bind(by)
    .fetch_one(pool)
    .await?)
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
    tracing::info!(%uid, from, to, action, "slur replaced");
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
    store::audit(pool, actor, action, Some(&uid.to_string()), json!({"from": from, "to": to, "reason": REASON})).await
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
                            tracing::info!(%uid, from = %name, to = kind, "slur in a display name replaced");
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
                            tracing::info!(%uid, from = %name, to = kind_text, "slur in a room name replaced");
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
                            tracing::info!(%uid, from = %code, to = %new, "slur in a connect code replaced");
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

    fn rule(id: i64, kind: RuleKind, body: &str) -> Rule {
        Rule {
            id,
            kind: kind.as_str().into(),
            body: body.into(),
            note: String::new(),
            enabled: true,
            created_by: "test".into(),
            created_at: chrono::Utc::now(),
            updated_at: chrono::Utc::now(),
        }
    }

    /// A local stand-in for DeepInfra: answers YES for texts containing "bad", "NO" otherwise
    /// (model `test/strict`, or a prompt saying "strict": always NO), and records the requests.
    async fn fake(
        answer_status: axum::http::StatusCode,
        rules: &[Rule],
    ) -> (SlurFilter, Arc<Mutex<Vec<serde_json::Value>>>) {
        type Seen = Arc<Mutex<Vec<serde_json::Value>>>;
        let seen: Seen = Default::default();
        let app = Router::new()
            .route(
                "/chat",
                post(move |State(seen): State<Seen>, Json(body): Json<serde_json::Value>| async move {
                    let text = body["messages"][1]["content"].as_str().unwrap_or_default().to_string();
                    let strict = body["model"] == "test/strict"
                        || body["messages"][0]["content"].as_str().unwrap_or_default().contains("strict");
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
        (SlurFilter::with_rules(&cfg, rules).unwrap(), seen)
    }

    #[tokio::test]
    async fn answers_are_read_and_cached() {
        let (f, seen) = fake(axum::http::StatusCode::OK, &[rule(1, RuleKind::Ai, "the prompt")]).await;
        assert!(f.is_slur(What::DisplayName, "so bad").await.unwrap());
        assert!(!f.is_slur(What::DisplayName, "Night").await.unwrap());
        assert!(f.is_slur(What::DisplayName, "SO BAD ").await.unwrap(), "cached case-insensitively");
        assert!(f.is_slur(What::CodeStart, "BAD").await.unwrap());
        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 3);
        assert_eq!(seen[0]["model"], "test/model");
        assert_eq!(seen[0]["messages"][0]["content"], "the prompt");
        assert_eq!(seen[0]["messages"][1]["content"], "display name: \"so bad\"");
        assert_eq!(seen[2]["messages"][1]["content"], "connect code start: \"BAD\"");
    }

    #[tokio::test]
    async fn screen_replaces_slurs_and_fails_open() {
        let ai = [rule(1, RuleKind::Ai, "the prompt")];
        let (f, _) = fake(axum::http::StatusCode::OK, &ai).await;
        assert_eq!(f.screen(What::DisplayName, "Night").await, Screened::Clean);
        assert!(matches!(f.screen(What::DisplayName, "bad").await, Screened::Replace(n) if KIND_NAMES.contains(&n)));
        assert!(matches!(f.screen(What::CodeStart, "BAD").await, Screened::Replace(p) if KIND_PREFIXES.contains(&p)));
        let (down, _) = fake(axum::http::StatusCode::SERVICE_UNAVAILABLE, &ai).await;
        assert_eq!(down.screen(What::DisplayName, "bad").await, Screened::Unchecked);
        // Without a key the ai rules are skipped; the word rules still count.
        let off =
            SlurFilter::with_rules(&SlurFilterConfig::default(), &[ai[0].clone(), rule(2, RuleKind::Word, "worse")])
                .unwrap();
        assert!(!off.enabled());
        assert_eq!(off.screen(What::DisplayName, "bad").await, Screened::Clean);
        assert!(matches!(off.screen(What::DisplayName, "w0r$e").await, Screened::Replace(_)));
        // No rules: nothing is flagged and nothing asked.
        let (none, seen) = fake(axum::http::StatusCode::OK, &[]).await;
        assert_eq!(none.screen(What::DisplayName, "bad").await, Screened::Clean);
        assert!(seen.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn every_model_must_agree() {
        let (mut f, seen) = fake(axum::http::StatusCode::OK, &[rule(1, RuleKind::Ai, "the prompt")]).await;
        f.models = vec!["test/model".into(), "test/strict".into()];
        assert!(!f.is_slur(What::DisplayName, "bad").await.unwrap());
        assert!(!f.is_slur(What::DisplayName, "Night").await.unwrap());
        let models: Vec<_> = seen.lock().unwrap().iter().map(|b| b["model"].as_str().unwrap().to_string()).collect();
        assert_eq!(models, ["test/model", "test/strict", "test/model"], "a NO ends the round");
    }

    #[tokio::test]
    async fn rules_combine() {
        let rules = [
            rule(1, RuleKind::Ai, "strict prompt"),
            rule(2, RuleKind::Ai, "the prompt"),
            rule(3, RuleKind::Word, "Gr1me"),
            rule(4, RuleKind::Regex, r"^zz\d+$"),
            rule(5, RuleKind::Allow, "Badminton"),
            rule(6, RuleKind::Allow, "  GRIME TIME "),
        ];
        let (f, seen) = fake(axum::http::StatusCode::OK, &rules).await;
        let set = f.rules().await.unwrap();
        // A word anywhere in the folded text, without asking the model.
        assert_eq!(f.check(&set, What::DisplayName, "xX g.r.!.m.3 Xx").await.unwrap(), Some(3));
        assert_eq!(f.check(&set, What::DisplayName, "zz123").await.unwrap(), Some(4));
        assert_eq!(f.check(&set, What::DisplayName, "zz12a").await.unwrap(), None);
        // Allowed texts are never flagged, by any rule.
        assert_eq!(f.check(&set, What::DisplayName, "badminton").await.unwrap(), None);
        assert_eq!(f.check(&set, What::DisplayName, "grime time").await.unwrap(), None);
        // The prompts in order: the first that says YES.
        assert_eq!(f.check(&set, What::DisplayName, "so bad").await.unwrap(), Some(2));
        let asked: Vec<_> = seen
            .lock()
            .unwrap()
            .iter()
            .map(|b| {
                let m = &b["messages"];
                format!("{} / {}", m[0]["content"].as_str().unwrap(), m[1]["content"].as_str().unwrap())
            })
            .collect();
        assert_eq!(
            asked,
            [
                "strict prompt / display name: \"zz12a\"",
                "the prompt / display name: \"zz12a\"",
                "strict prompt / display name: \"so bad\"",
                "the prompt / display name: \"so bad\"",
            ]
        );
    }

    #[test]
    fn folding_and_checks() {
        assert_eq!(fold("G 1 r-M @_$"), "girmas");
        assert_eq!(fold("Ünï 3"), "ünïe");
        assert_eq!(prompt_hash(&["a".into()], "p"), prompt_hash(&["a".into()], "p"));
        assert_ne!(prompt_hash(&["a".into()], "p"), prompt_hash(&["b".into()], "p"));
        assert!(check_rule(RuleKind::Regex, "(unclosed").is_err());
        assert!(check_rule(RuleKind::Word, "!!").is_ok(), "folds to ii");
        assert!(check_rule(RuleKind::Word, "--").is_err());
        assert!(check_rule(RuleKind::Ai, "  ").is_err());
        assert_eq!(What::parse("room"), Some(What::RoomName));
        assert_eq!(RuleKind::parse("allow"), Some(RuleKind::Allow));
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
