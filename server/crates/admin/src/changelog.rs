//! `admin changelog`: the launcher's changelogs (docs/design/changelog.md, `accounts::changelog`).
//! Sarah's agent board runs `publish` with the changelog she wrote there, on prod just before it
//! pushes main (`ssh … sudo pp-admin changelog publish`) and on staging before a staging run when
//! "show on staging" is on (`staging.py admin changelog publish --every-start`), and `render` for
//! its preview.

use std::io::Read;
use std::path::PathBuf;

use accounts::changelog::{self, MediaFile};
use accounts::store;
use anyhow::Context;
use base64::Engine;
use clap::Subcommand;
use serde::Deserialize;
use serde_json::json;
use sqlx::PgPool;

#[derive(Subcommand)]
pub enum ChangelogCmd {
    /// Publish a changelog, read as JSON from stdin: {"markdown": "...", "media": [{"name":
    /// "<20 hex>.<ext>", "data": "<base64>"}]}. Its pictures go to the blog's media folder, where
    /// the Markdown finds them as /blog/media/<name>. Launchers updated to a release built after
    /// this show it once.
    Publish {
        /// Staging only: shown at every start of every launcher, replacing the last such one.
        #[arg(long)]
        every_start: bool,
        /// Where accounts serves /blog/media/ from (as accounts reads it; relative to here).
        #[arg(long, env = "BLOG_MEDIA_DIR", default_value = "blog-media")]
        media_dir: PathBuf,
        /// Date it then (RFC 3339, at most 2 days ago) instead of now: the board's first push with
        /// patch notes publishes after that push deployed this command, dated just before it.
        #[arg(long)]
        published_at: Option<String>,
        /// EmissaryRecomp's patch notes for this release (`v0.7.2`), not the launcher's: the board
        /// publishes them once the release is live; launchers that play with it show them.
        #[arg(long)]
        emissary: Option<String>,
    },
    /// Remove the --every-start one (staging's "show on staging" turned off).
    ClearEveryStart,
    /// Every changelog, newest first: id, date and first line.
    List,
    /// Markdown from stdin to the HTML launchers get, on stdout (no database needed).
    Render,
}

#[derive(Deserialize)]
struct Bundle {
    markdown: String,
    #[serde(default)]
    media: Vec<BundleMedia>,
}

#[derive(Deserialize)]
struct BundleMedia {
    name: String,
    data: String,
}

fn read_stdin() -> anyhow::Result<String> {
    let mut s = String::new();
    std::io::stdin().read_to_string(&mut s).context("reading stdin")?;
    Ok(s)
}

/// `render` needs no database: done before main connects.
pub fn render() -> anyhow::Result<()> {
    print!("{}", accounts::blog::render(&read_stdin()?).html);
    Ok(())
}

pub async fn run(pool: &PgPool, actor: &str, cmd: ChangelogCmd) -> anyhow::Result<()> {
    match cmd {
        ChangelogCmd::Publish { every_start, media_dir, published_at, emissary } => {
            let at = published_at
                .map(|s| {
                    chrono::DateTime::parse_from_rfc3339(s.trim())
                        .map(|t| t.with_timezone(&chrono::Utc))
                        .with_context(|| format!("--published-at {s:?}: not an RFC 3339 time"))
                })
                .transpose()?;
            let bundle: Bundle = serde_json::from_str(&read_stdin()?).context("stdin is not a changelog bundle")?;
            let media = bundle
                .media
                .into_iter()
                .map(|m| {
                    let data = base64::engine::general_purpose::STANDARD
                        .decode(m.data.as_bytes())
                        .with_context(|| format!("{}: bad base64", m.name))?;
                    Ok(MediaFile { name: m.name, data })
                })
                .collect::<anyhow::Result<Vec<_>>>()?;
            let emissary = emissary.as_deref().map(str::trim);
            let p = changelog::publish_for(pool, &media_dir, &bundle.markdown, &media, every_start, at, emissary).await?;
            let action = if every_start { "changelog.publish_every_start" } else { "changelog.publish" };
            store::audit(
                pool,
                actor,
                action,
                Some(&p.id.to_string()),
                json!({ "media": media.len(), "written": p.written, "emissary": emissary }),
            )
            .await?;
            println!(
                "published {}changelog {} at {}{} ({} media file(s), {} new)",
                emissary.map(|v| format!("EmissaryRecomp {v} ")).unwrap_or_default(),
                p.id,
                p.published_at.format("%Y-%m-%d %H:%M:%S UTC"),
                if every_start { ", shown at every start" } else { "" },
                media.len(),
                p.written.len()
            );
        }
        ChangelogCmd::ClearEveryStart => {
            let n = changelog::clear_every_start(pool).await?;
            println!("removed {n} every-start changelog(s)");
        }
        ChangelogCmd::List => {
            let rows = changelog::list(pool).await?;
            for r in &rows {
                println!(
                    "#{}  {}{}{}  {}",
                    r.id,
                    r.published_at.format("%Y-%m-%d %H:%M UTC"),
                    if r.every_start { "  [every start]" } else { "" },
                    r.version.as_deref().map(|v| format!("  [emissary {v}]")).unwrap_or_default(),
                    changelog::title_of(&r.body_md)
                );
            }
            println!("{} changelog(s)", rows.len());
        }
        ChangelogCmd::Render => render()?,
    }
    Ok(())
}
