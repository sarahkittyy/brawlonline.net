//! `admin filter`: the name filter's rules and samples (`accounts::slur_filter`), kept in the
//! database so the prompt and the words stay out of the repository. New rules start off (a
//! draft); `filter test` shows what turning them on would change over every name players have
//! now, the texts the filter replaced before and the samples, before `filter on` makes them live
//! (accounts reads the rules every 30 s).

use std::collections::{BTreeMap, HashMap};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use accounts::config::SlurFilterConfig;
use accounts::slur_filter::{self as sf, Local, Rule, RuleKind, RuleSet, Sample, SlurFilter, What};
use accounts::store;
use anyhow::Context;
use clap::Subcommand;
use serde_json::json;
use sqlx::PgPool;

#[derive(Subcommand)]
pub enum FilterCmd {
    /// Every rule: its id, on or off, kind, the word, regex or allowed text (an ai rule's first
    /// line), and its note.
    List,
    /// One rule in full (an ai rule's whole prompt).
    Show { id: i64 },
    /// Add a rule, off (a draft) unless --on. Kinds: `ai` (a prompt for the model, which answers
    /// YES or NO per text), `word` (found anywhere in the text once folded: any case, 1 and ! read
    /// as i, 3 as e, @ and 4 as a, $ and 5 as s ..., spaces and symbols left out), `regex` (case
    /// insensitive, on the text as typed and folded), `allow` (this exact text, any case, is never
    /// flagged: the fix for a false positive).
    Add {
        #[arg(value_parser = ["ai", "word", "regex", "allow"])]
        kind: String,
        /// The prompt, word, regex or text; "-" reads it from stdin. Or --file.
        body: Option<String>,
        /// Read the rule from this file (a prompt).
        #[arg(long)]
        file: Option<PathBuf>,
        #[arg(long, default_value = "")]
        note: String,
        /// Live at once, without a test first.
        #[arg(long)]
        on: bool,
    },
    /// A draft copy of a rule, to change and test while the rule itself stays as it is; then
    /// `filter on COPY --instead-of ID`.
    Copy {
        id: i64,
        #[arg(long)]
        note: Option<String>,
    },
    /// Change a rule's text (BODY, --file, or your $EDITOR when neither is given) or its note.
    /// A rule that is on changes for players within 30 s, so that needs --live (better: `copy`).
    Edit {
        id: i64,
        /// The new text; "-" reads it from stdin.
        body: Option<String>,
        #[arg(long)]
        file: Option<PathBuf>,
        #[arg(long)]
        note: Option<String>,
        /// Change a rule that is on.
        #[arg(long)]
        live: bool,
    },
    /// Turn rules on, and with --instead-of turn others off in the same step.
    On {
        #[arg(required = true)]
        ids: Vec<i64>,
        #[arg(long, num_args = 1..)]
        instead_of: Vec<i64>,
    },
    /// Turn rules off (they stay as drafts).
    Off {
        #[arg(required = true)]
        ids: Vec<i64>,
    },
    /// Delete a rule.
    Rm { id: i64 },
    /// What turning rules IDS on (and --instead-of ones off) would change, over every display
    /// name, code start, room name and social media username players have now, every text the
    /// filter replaced before, and the samples. Without IDS: what the rules on now flag. The
    /// model's answers are kept per prompt, so only texts a prompt has not seen are asked
    /// (about US$0.00002 each): a new prompt costs some cents the first time, a word or regex
    /// nothing.
    Test {
        ids: Vec<i64>,
        #[arg(long, num_args = 1..)]
        instead_of: Vec<i64>,
        /// Also print every text flagged with the change, not only what changes.
        #[arg(long)]
        list: bool,
        /// Stop before asking the model more often than this.
        #[arg(long, default_value_t = 20_000)]
        max_asks: usize,
        /// Requests to DeepInfra at once.
        #[arg(long, default_value_t = 16)]
        concurrency: usize,
        #[command(flatten)]
        filter: Box<SlurFilterConfig>,
    },
    /// Samples: texts `filter test` expects flagged or passed, besides the players' names.
    #[command(subcommand)]
    Sample(SampleCmd),
    /// Every rule and sample as JSON (a backup, or to move them between staging and prod).
    Export,
    /// Add the rules and samples of an `export` file; a rule keeps its on or off, one already
    /// there (same kind and text) is left alone.
    Import { file: PathBuf },
}

#[derive(Subcommand)]
pub enum SampleCmd {
    /// Add a sample (or change what it expects).
    Add {
        #[arg(value_parser = ["flag", "pass"])]
        expect: String,
        text: String,
        #[arg(long, default_value = "name", value_parser = ["name", "code", "room", "social"])]
        what: String,
        #[arg(long, default_value = "")]
        note: String,
    },
    List,
    Rm {
        id: i64,
    },
}

#[derive(serde::Serialize, serde::Deserialize)]
struct Export {
    rules: Vec<Rule>,
    samples: Vec<Sample>,
}

pub async fn run(pool: &PgPool, actor: &str, cmd: FilterCmd) -> anyhow::Result<()> {
    match cmd {
        FilterCmd::List => {
            let rules = sf::load_rules(pool, false).await?;
            if rules.is_empty() {
                println!("no rules: the filter flags nothing (filter add, or filter import)");
            }
            for r in &rules {
                println!("{}", rule_line(r));
            }
        }
        FilterCmd::Show { id } => {
            let r = rule(pool, id).await?;
            println!("{}", rule_line(&r));
            println!(
                "added {} by {}, changed {}",
                r.created_at.format("%Y-%m-%d %H:%M"),
                r.created_by,
                r.updated_at.format("%Y-%m-%d %H:%M")
            );
            println!("---\n{}", r.body);
        }
        FilterCmd::Add { kind, body, file, note, on } => {
            let kind = RuleKind::parse(&kind).unwrap();
            let body = read_body(body, file)?.context("give the rule as BODY, --file or - (stdin)")?;
            let id = sf::add_rule(pool, kind, &body, &note, on, actor).await?;
            store::audit(
                pool,
                actor,
                "filter.add",
                Some(&id.to_string()),
                json!({"kind": kind.as_str(), "body": body, "note": note, "enabled": on}),
            )
            .await?;
            if on {
                println!("rule #{id} added and on (accounts uses it within 30 s)");
            } else {
                println!("rule #{id} added, off: `filter test {id}` shows what it would change, `filter on {id}` turns it on");
            }
        }
        FilterCmd::Copy { id, note } => {
            let r = rule(pool, id).await?;
            let kind = RuleKind::parse(&r.kind).context("unknown kind")?;
            let note =
                note.unwrap_or_else(|| format!("copy of #{id}{}", if r.note.is_empty() { "" } else { ": " }) + &r.note);
            let new = sf::add_rule(pool, kind, &r.body, &note, false, actor).await?;
            store::audit(
                pool,
                actor,
                "filter.add",
                Some(&new.to_string()),
                json!({"kind": r.kind, "copy_of": id, "note": note}),
            )
            .await?;
            println!("rule #{new} is an off copy of #{id}: `filter edit {new}`, `filter test {new} --instead-of {id}`, then `filter on {new} --instead-of {id}`");
        }
        FilterCmd::Edit { id, body, file, note, live } => {
            let r = rule(pool, id).await?;
            anyhow::ensure!(
                !r.enabled || live,
                "rule #{id} is on: a change reaches players within 30 s. Add --live, or `filter copy {id}` and test the copy"
            );
            let kind = RuleKind::parse(&r.kind).context("unknown kind")?;
            let body = match read_body(body, file)? {
                Some(b) => b,
                None if note.is_some() => r.body.clone(),
                None => edit_in_editor(&r.body)?,
            };
            sf::check_rule(kind, &body)?;
            let note = note.unwrap_or_else(|| r.note.clone());
            if body == r.body && note == r.note {
                println!("rule #{id} unchanged");
                return Ok(());
            }
            sqlx::query("UPDATE name_filter_rules SET body = $2, note = $3, updated_at = now() WHERE id = $1")
                .bind(id)
                .bind(&body)
                .bind(&note)
                .execute(pool)
                .await?;
            store::audit(
                pool,
                actor,
                "filter.edit",
                Some(&id.to_string()),
                json!({"from": r.body, "to": body, "note_from": r.note, "note": note}),
            )
            .await?;
            println!("rule #{id} changed{}", if r.enabled { " (live within 30 s)" } else { "" });
        }
        FilterCmd::On { ids, instead_of } => {
            for id in ids.iter().chain(&instead_of) {
                rule(pool, *id).await?;
            }
            let mut tx = pool.begin().await?;
            sqlx::query("UPDATE name_filter_rules SET enabled = true, updated_at = now() WHERE id = ANY($1)")
                .bind(&ids)
                .execute(&mut *tx)
                .await?;
            sqlx::query("UPDATE name_filter_rules SET enabled = false, updated_at = now() WHERE id = ANY($1)")
                .bind(&instead_of)
                .execute(&mut *tx)
                .await?;
            tx.commit().await?;
            store::audit(pool, actor, "filter.on", None, json!({"on": ids, "off": instead_of})).await?;
            println!("on: {}{} (accounts uses it within 30 s)", ids_text(&ids), off_text(&instead_of));
        }
        FilterCmd::Off { ids } => {
            for id in &ids {
                rule(pool, *id).await?;
            }
            sqlx::query("UPDATE name_filter_rules SET enabled = false, updated_at = now() WHERE id = ANY($1)")
                .bind(&ids)
                .execute(pool)
                .await?;
            store::audit(pool, actor, "filter.off", None, json!({"off": ids})).await?;
            println!("off: {} (within 30 s)", ids_text(&ids));
        }
        FilterCmd::Rm { id } => {
            let r = rule(pool, id).await?;
            sqlx::query("DELETE FROM name_filter_rules WHERE id = $1").bind(id).execute(pool).await?;
            store::audit(
                pool,
                actor,
                "filter.rm",
                Some(&id.to_string()),
                json!({"kind": r.kind, "body": r.body, "note": r.note, "enabled": r.enabled}),
            )
            .await?;
            println!("rule #{id} deleted");
        }
        FilterCmd::Test { ids, instead_of, list, max_asks, concurrency, filter } => {
            let filter = Arc::new(SlurFilter::new(&filter, pool.clone())?);
            test(pool, filter, &ids, &instead_of, list, max_asks, concurrency.max(1)).await?;
        }
        FilterCmd::Sample(SampleCmd::Add { expect, text, what, note }) => {
            let what = What::parse(&what).unwrap();
            let expect_flag = expect == "flag";
            let id = sf::add_sample(pool, what, &text, expect_flag, &note, actor).await?;
            store::audit(
                pool,
                actor,
                "filter.sample_add",
                Some(&id.to_string()),
                json!({"what": what.key(), "text": text, "expect_flag": expect_flag}),
            )
            .await?;
            println!("sample #{id}: {} {:?} should {expect}", what.key(), text);
        }
        FilterCmd::Sample(SampleCmd::List) => {
            let samples = sf::load_samples(pool).await?;
            if samples.is_empty() {
                println!("no samples");
            }
            for s in &samples {
                println!(
                    "#{:<4} {}  {:<6} {:?}{}",
                    s.id,
                    if s.expect_flag { "flag" } else { "pass" },
                    s.what,
                    s.text,
                    if s.note.is_empty() { String::new() } else { format!("  ({})", s.note) }
                );
            }
        }
        FilterCmd::Sample(SampleCmd::Rm { id }) => {
            let gone = sqlx::query("DELETE FROM name_filter_samples WHERE id = $1").bind(id).execute(pool).await?;
            anyhow::ensure!(gone.rows_affected() == 1, "no sample #{id}");
            store::audit(pool, actor, "filter.sample_rm", Some(&id.to_string()), json!({})).await?;
            println!("sample #{id} deleted");
        }
        FilterCmd::Export => {
            let out = Export { rules: sf::load_rules(pool, false).await?, samples: sf::load_samples(pool).await? };
            println!("{}", serde_json::to_string_pretty(&out)?);
        }
        FilterCmd::Import { file } => {
            let text = std::fs::read_to_string(&file).with_context(|| format!("reading {}", file.display()))?;
            let import: Export = serde_json::from_str(&text).context("not a `filter export` file")?;
            let have = sf::load_rules(pool, false).await?;
            let (mut added, mut skipped) = (Vec::new(), 0);
            for r in &import.rules {
                let kind = RuleKind::parse(&r.kind).with_context(|| format!("unknown kind {:?}", r.kind))?;
                if have.iter().any(|h| h.kind == r.kind && h.body == r.body) {
                    skipped += 1;
                    continue;
                }
                added.push(sf::add_rule(pool, kind, &r.body, &r.note, r.enabled, actor).await?);
            }
            for s in &import.samples {
                let what = What::parse(&s.what).with_context(|| format!("unknown sample kind {:?}", s.what))?;
                sf::add_sample(pool, what, &s.text, s.expect_flag, &s.note, actor).await?;
            }
            store::audit(
                pool,
                actor,
                "filter.import",
                None,
                json!({"file": file.display().to_string(), "rules": added, "samples": import.samples.len()}),
            )
            .await?;
            println!(
                "{} rule(s) added ({}), {skipped} already there; {} sample(s)",
                added.len(),
                ids_text(&added),
                import.samples.len()
            );
        }
    }
    Ok(())
}

async fn rule(pool: &PgPool, id: i64) -> anyhow::Result<Rule> {
    sf::get_rule(pool, id).await?.with_context(|| format!("no rule #{id} (filter list)"))
}

fn rule_line(r: &Rule) -> String {
    let body = if r.kind == "ai" {
        let first: String = r.body.lines().next().unwrap_or_default().chars().take(70).collect();
        format!("prompt, {} chars: {first:?}...", r.body.chars().count())
    } else {
        format!("{:?}", r.body)
    };
    let note = if r.note.is_empty() { String::new() } else { format!("  ({})", r.note) };
    format!("#{:<4} {:<3} {:<5} {body}{note}", r.id, if r.enabled { "on" } else { "off" }, r.kind)
}

fn ids_text(ids: &[i64]) -> String {
    ids.iter().map(|i| format!("#{i}")).collect::<Vec<_>>().join(", ")
}

fn off_text(ids: &[i64]) -> String {
    if ids.is_empty() {
        String::new()
    } else {
        format!("; off: {}", ids_text(ids))
    }
}

/// BODY ("-": stdin) or --file, trimmed; none when neither is given.
fn read_body(body: Option<String>, file: Option<PathBuf>) -> anyhow::Result<Option<String>> {
    let text = match (body, file) {
        (Some(_), Some(_)) => anyhow::bail!("give BODY or --file, not both"),
        (Some(b), None) if b == "-" => std::io::read_to_string(std::io::stdin())?,
        (Some(b), None) => b,
        (None, Some(f)) => std::fs::read_to_string(&f).with_context(|| format!("reading {}", f.display()))?,
        (None, None) => return Ok(None),
    };
    Ok(Some(text.trim().to_string()))
}

/// `text` in $VISUAL or $EDITOR (nano without either), as saved.
fn edit_in_editor(text: &str) -> anyhow::Result<String> {
    let editor = std::env::var("VISUAL").or_else(|_| std::env::var("EDITOR")).unwrap_or_else(|_| {
        if cfg!(windows) {
            "notepad".into()
        } else {
            "nano".into()
        }
    });
    let path = std::env::temp_dir().join(format!("pp-filter-rule-{}.txt", std::process::id()));
    std::fs::write(&path, text)?;
    let mut parts = editor.split_whitespace();
    let status = std::process::Command::new(parts.next().context("empty $EDITOR")?).args(parts).arg(&path).status();
    let edited = std::fs::read_to_string(&path);
    let _ = std::fs::remove_file(&path);
    anyhow::ensure!(status.with_context(|| format!("running {editor}"))?.success(), "{editor} failed");
    Ok(edited?.trim().to_string())
}

/// A text `test` checks, with where it comes from.
struct Entry {
    what: What,
    text: String,
    key: String,
    players: usize,
    example: Option<String>,
    replaced: bool,
    expect: Option<bool>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Verdict {
    Flag(i64),
    Pass,
    /// The model did not answer.
    Unknown,
}

impl Verdict {
    fn flagged(self) -> bool {
        matches!(self, Verdict::Flag(_))
    }
}

type Answers = HashMap<(String, What, String), bool>;

fn decide(set: &RuleSet, e: &Entry, answers: &Answers) -> Verdict {
    match set.local(&e.text) {
        Local::Allowed(_) => Verdict::Pass,
        Local::Flagged(id) => Verdict::Flag(id),
        Local::AskModel => {
            let mut unknown = false;
            for p in &set.prompts {
                match answers.get(&(p.hash.clone(), e.what, e.key.clone())) {
                    Some(true) => return Verdict::Flag(p.rule),
                    Some(false) => {}
                    None => unknown = true,
                }
            }
            if unknown {
                Verdict::Unknown
            } else {
                Verdict::Pass
            }
        }
    }
}

/// Every text players have now (names, code starts, room names, social media usernames), the
/// texts the filter replaced before, and the samples, once each per kind (any case).
async fn corpus(pool: &PgPool) -> anyhow::Result<Vec<Entry>> {
    let mut by_key: BTreeMap<(What, String), Entry> = BTreeMap::new();
    fn put<'m>(
        by_key: &'m mut BTreeMap<(What, String), Entry>,
        what: What,
        text: &str,
        code: Option<&str>,
    ) -> Option<&'m mut Entry> {
        let text = text.trim();
        if text.is_empty() {
            return None;
        }
        let key = sf::answer_key(text);
        let e = by_key.entry((what, key.clone())).or_insert_with(|| Entry {
            what,
            text: text.to_string(),
            key,
            players: 0,
            example: None,
            replaced: false,
            expect: None,
        });
        if let Some(code) = code {
            e.players += 1;
            e.example.get_or_insert_with(|| code.to_string());
        }
        Some(e)
    }
    let mut add = |what: What, text: &str, code: Option<&str>| put(&mut by_key, what, text, code).map(|_| ());
    type UserNames = (String, Option<String>, Option<String>, Option<String>);
    let users: Vec<UserNames> =
        sqlx::query_as("SELECT display_name, connect_code, room_name, mods_room_name FROM users")
            .fetch_all(pool)
            .await?;
    for (name, code, room, mods_room) in &users {
        let code_text = code.as_deref().unwrap_or("(no code)");
        add(What::DisplayName, name, Some(code_text));
        if let Some((start, _)) = code.as_deref().and_then(|c| c.split_once('#')) {
            add(What::CodeStart, start, Some(code_text));
        }
        for room in [room, mods_room].into_iter().flatten() {
            add(What::RoomName, room, Some(code_text));
        }
    }
    type Socials = (
        Option<String>,
        Option<String>,
        Option<String>,
        Option<String>,
        Option<String>,
        Option<String>,
        Option<String>,
    );
    let socials: Vec<Socials> = sqlx::query_as(
        "SELECT u.connect_code, p.discord, p.twitter, p.twitch, p.youtube, p.tiktok, p.instagram
           FROM profiles p JOIN users u USING (uid)",
    )
    .fetch_all(pool)
    .await?;
    for (code, a, b, c, d, e, f) in &socials {
        let code_text = code.as_deref().unwrap_or("(no code)");
        for h in [a, b, c, d, e, f].into_iter().flatten() {
            add(What::SocialHandle, h, Some(code_text));
        }
    }
    let replaced: Vec<(String, Option<String>)> = sqlx::query_as(
        "SELECT action, detail->>'from' FROM audit_log WHERE detail->>'reason' IN ('slur', 'racial slur')",
    )
    .fetch_all(pool)
    .await?;
    for (action, from) in &replaced {
        let Some(from) = from else { continue };
        let (what, text) = match action.as_str() {
            "user.set_code" => (What::CodeStart, from.split_once('#').map_or(from.as_str(), |(p, _)| p)),
            "user.room-name" => (What::RoomName, from.as_str()),
            _ => (What::DisplayName, from.as_str()),
        };
        if let Some(e) = put(&mut by_key, what, text, None) {
            e.replaced = true;
        }
    }
    for s in sf::load_samples(pool).await? {
        let Some(what) = What::parse(&s.what) else { continue };
        if let Some(e) = put(&mut by_key, what, &s.text, None) {
            e.expect = Some(s.expect_flag);
        }
    }
    Ok(by_key.into_values().collect())
}

async fn test(
    pool: &PgPool,
    filter: Arc<SlurFilter>,
    ids: &[i64],
    instead_of: &[i64],
    list: bool,
    max_asks: usize,
    concurrency: usize,
) -> anyhow::Result<()> {
    let all = sf::load_rules(pool, false).await?;
    for id in ids.iter().chain(instead_of) {
        anyhow::ensure!(all.iter().any(|r| r.id == *id), "no rule #{id} (filter list)");
    }
    let live_rules: Vec<&Rule> = all.iter().filter(|r| r.enabled).collect();
    let cand_rules: Vec<&Rule> =
        all.iter().filter(|r| (r.enabled && !instead_of.contains(&r.id)) || ids.contains(&r.id)).collect();
    let live = RuleSet::compile(live_rules.iter().copied(), filter.models());
    let cand = RuleSet::compile(cand_rules.iter().copied(), filter.models());
    let changing = !ids.is_empty() || !instead_of.is_empty();
    let entries = corpus(pool).await?;

    // The model's answers: the kept ones, then the missing ones asked.
    let mut prompts: Vec<&sf::Prompt> = Vec::new();
    for p in live.prompts.iter().chain(&cand.prompts) {
        if !prompts.iter().any(|q| q.hash == p.hash) {
            prompts.push(p);
        }
    }
    let mut answers: Answers = HashMap::new();
    for p in &prompts {
        let rows: Vec<(String, String, bool)> =
            sqlx::query_as("SELECT what, text, flagged FROM name_filter_answers WHERE prompt_hash = $1")
                .bind(&p.hash)
                .fetch_all(pool)
                .await?;
        for (what, text, flagged) in rows {
            if let Some(what) = What::parse(&what) {
                answers.insert((p.hash.clone(), what, text), flagged);
            }
        }
    }
    let mut to_ask: Vec<(&sf::Prompt, &Entry)> = Vec::new();
    let mut queued = std::collections::HashSet::new();
    for e in &entries {
        for set in [&live, &cand] {
            if set.local(&e.text) != Local::AskModel {
                continue;
            }
            for p in &set.prompts {
                let key = (p.hash.clone(), e.what, e.key.clone());
                if !answers.contains_key(&key) && queued.insert(key) {
                    to_ask.push((p, e));
                }
            }
        }
    }
    if !to_ask.is_empty() && !filter.enabled() {
        eprintln!("no DEEPINFRA_API_KEY: {} text(s) the model has not seen are left without an answer", to_ask.len());
        to_ask.clear();
    }
    if !to_ask.is_empty() {
        anyhow::ensure!(
            to_ask.len() <= max_asks,
            "{} texts need the model, more than --max-asks {max_asks}; raise it to go on",
            to_ask.len()
        );
        let tokens: usize =
            to_ask.iter().map(|(p, e)| (p.text.chars().count() + e.text.chars().count() + 40) / 4).sum::<usize>()
                * filter.models().len();
        eprintln!(
            "asking the model about {} text(s) it has not seen with this prompt: at most ~{tokens} tokens (~US${:.2} at US$0.075 a million)",
            to_ask.len(),
            tokens as f64 * 0.075 / 1e6
        );
        let gate = Arc::new(tokio::sync::Semaphore::new(concurrency));
        let mut asks = tokio::task::JoinSet::new();
        let total = to_ask.len();
        for (p, e) in &to_ask {
            let (gate, filter, p, what, text) = (gate.clone(), filter.clone(), (*p).clone(), e.what, e.text.clone());
            asks.spawn(async move {
                let _permit = gate.acquire_owned().await;
                let mut answer = filter.ask(&p, what, &text).await;
                for _ in 0..2 {
                    if answer.is_ok() {
                        break;
                    }
                    tokio::time::sleep(Duration::from_secs(2)).await;
                    answer = filter.ask(&p, what, &text).await;
                }
                (p.hash, what, sf::answer_key(&text), text, answer)
            });
        }
        let mut done = 0;
        while let Some(r) = asks.join_next().await {
            let (hash, what, key, text, answer) = r?;
            done += 1;
            if done % 500 == 0 {
                eprintln!("{done}/{total}");
            }
            match answer {
                Ok(hit) => {
                    answers.insert((hash, what, key), hit);
                }
                Err(e) => eprintln!("{} {text:?}: no answer ({e:#})", what.key()),
            }
        }
    }

    // What each set says.
    let verdicts: Vec<(Verdict, Verdict)> =
        entries.iter().map(|e| (decide(&live, e, &answers), decide(&cand, e, &answers))).collect();
    let players = entries.iter().filter(|e| e.players > 0).count();
    let count = |w: What| entries.iter().filter(|e| e.players > 0 && e.what == w).count();
    println!(
        "{} texts: {players} from players ({} names, {} code starts, {} room names, {} social media), {} replaced before, {} samples",
        entries.len(),
        count(What::DisplayName),
        count(What::CodeStart),
        count(What::RoomName),
        count(What::SocialHandle),
        entries.iter().filter(|e| e.replaced).count(),
        entries.iter().filter(|e| e.expect.is_some()).count(),
    );
    println!(
        "on now: {}",
        if live_rules.is_empty() {
            "nothing".into()
        } else {
            ids_text(&live_rules.iter().map(|r| r.id).collect::<Vec<_>>())
        }
    );
    let flagged_live = verdicts.iter().filter(|v| v.0.flagged()).count();
    if changing {
        println!("trying: on {}{}", if ids.is_empty() { "-".into() } else { ids_text(ids) }, off_text(instead_of));
        let flagged_cand = verdicts.iter().filter(|v| v.1.flagged()).count();
        println!("flagged now: {flagged_live}; with the change: {flagged_cand}");
        print_section(
            "newly flagged",
            entries.iter().zip(&verdicts).filter(|(_, v)| v.1.flagged() && !v.0.flagged()).map(|(e, v)| (e, v.1)),
        );
        print_section(
            "no longer flagged",
            entries.iter().zip(&verdicts).filter(|(_, v)| v.0.flagged() && v.1 == Verdict::Pass).map(|(e, v)| (e, v.0)),
        );
        print_section(
            "samples wrong now",
            entries.iter().zip(&verdicts).filter(|(e, v)| wrong(e, v.0)).map(|(e, v)| (e, v.0)),
        );
        print_section(
            "samples wrong with the change",
            entries.iter().zip(&verdicts).filter(|(e, v)| wrong(e, v.1)).map(|(e, v)| (e, v.1)),
        );
        if list {
            print_section(
                "flagged with the change",
                entries.iter().zip(&verdicts).filter(|(_, v)| v.1.flagged()).map(|(e, v)| (e, v.1)),
            );
        }
    } else {
        print_section(
            "flagged now",
            entries.iter().zip(&verdicts).filter(|(_, v)| v.0.flagged()).map(|(e, v)| (e, v.0)),
        );
        print_section(
            "samples wrong",
            entries.iter().zip(&verdicts).filter(|(e, v)| wrong(e, v.0)).map(|(e, v)| (e, v.0)),
        );
    }
    let unknown = verdicts.iter().filter(|v| v.0 == Verdict::Unknown || v.1 == Verdict::Unknown).count();
    if unknown > 0 {
        println!("{unknown} text(s) without the model's answer: run the test again to ask them");
    }
    Ok(())
}

/// A sample the verdict gets wrong (unanswered ones are counted at the end instead).
fn wrong(e: &Entry, v: Verdict) -> bool {
    v != Verdict::Unknown && e.expect.is_some_and(|flag| flag != v.flagged())
}

fn print_section<'a>(title: &str, rows: impl Iterator<Item = (&'a Entry, Verdict)>) {
    let rows: Vec<_> = rows.collect();
    println!("{title} ({}):", rows.len());
    for (e, v) in rows {
        let by = match v {
            Verdict::Flag(id) => format!("by #{id}"),
            Verdict::Pass => "passes".into(),
            Verdict::Unknown => "no answer".into(),
        };
        let mut from = Vec::new();
        if e.players > 0 {
            from.push(format!(
                "{} player{}, e.g. {}",
                e.players,
                if e.players == 1 { "" } else { "s" },
                e.example.as_deref().unwrap_or("-")
            ));
        }
        if e.replaced {
            from.push("replaced before".into());
        }
        if let Some(flag) = e.expect {
            from.push(format!("sample: should {}", if flag { "flag" } else { "pass" }));
        }
        println!("  {:<6} {:<34} {by:<10} {}", e.what.key(), format!("{:?}", e.text), from.join("; "));
    }
}
