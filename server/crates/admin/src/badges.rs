//! `admin icon`, `admin badge`, `admin main-icon`: badges before players' names and the icon in
//! place of their primary main (docs/design/profiles.md §10, `accounts::icons`). Upload a PNG once
//! with `icon add`, then give it to players by its name. Changes show within seconds around the
//! launcher (accounts' account watcher lists the player again) and on the profile page.

use std::path::PathBuf;

use accounts::icons::{self, IconRow};
use accounts::store::{self, UserRow};
use anyhow::Context;
use clap::Subcommand;
use common::badges::{icon_path, parse_places, Place, MAX_ICON_BYTES, MAX_ICON_SIDE};
use serde_json::json;
use sqlx::PgPool;

#[derive(Subcommand)]
pub enum IconCmd {
    /// Upload a PNG icon (animated too; at most 64 KiB and 256x256) under NAME (a-z, 0-9, -),
    /// for `badge add` and `main-icon set`. The file is read on this machine.
    Add {
        file: PathBuf,
        /// What to call it (default: the file's name without .png).
        #[arg(long)]
        name: Option<String>,
    },
    /// Every icon: its name, size, id (its address is /v1/icons/<id>.png) and how many players
    /// have it as a badge and as their main icon.
    List,
    /// Delete an icon. While players have it, only with --force, which takes it off them.
    Remove {
        name: String,
        #[arg(long)]
        force: bool,
    },
}

#[derive(Subcommand)]
pub enum BadgeCmd {
    /// Give a player (IDENT: a uid, email or connect code) a badge before their name, or change
    /// the one they have with that icon. A new badge goes last, with no hover text, everywhere,
    /// unless the options say otherwise.
    Add {
        ident: String,
        /// The icon's name (`icon list`).
        icon: String,
        /// The hover text (at most 60 characters); "" for none.
        #[arg(long)]
        label: Option<String>,
        /// Where it shows: `all`, or a comma-separated list of profile (the launcher's profile and
        /// the web page), hover (the small profile on hovering a name), online (online list and
        /// Friends), chat, leaderboard, rooms (P+ rooms and Other Games hosts).
        #[arg(long)]
        places: Option<String>,
        /// Its place among the player's badges, 1 = first (nearest the left).
        #[arg(long)]
        pos: Option<usize>,
    },
    /// A player's badges, in order, and their main icon.
    List { ident: String },
    /// Take a badge off a player.
    Remove { ident: String, icon: String },
}

#[derive(Subcommand)]
pub enum MainIconCmd {
    /// Show this icon wherever the player's primary main's stock icon shows beside their name,
    /// whatever their own "Show next to name" switch says.
    Set {
        ident: String,
        icon: String,
        /// The hover text (at most 60 characters), as a stock icon shows its character's name.
        #[arg(long, default_value = "")]
        label: String,
    },
    /// Back to the primary main's stock icon.
    Clear { ident: String },
}

fn who(u: &UserRow) -> String {
    format!("{} ({})", u.display_name, u.connect_code.as_deref().unwrap_or("-"))
}

fn places_text(places: &Option<Vec<String>>) -> String {
    match places {
        None => "everywhere".into(),
        Some(p) => p.join(","),
    }
}

pub async fn icon(pool: &PgPool, actor: &str, cmd: IconCmd) -> anyhow::Result<()> {
    match cmd {
        IconCmd::Add { file, name } => {
            let name = match name {
                Some(n) => n,
                None => file
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .map(|s| s.to_ascii_lowercase().replace([' ', '_'], "-"))
                    .unwrap_or_default(),
            };
            let meta = std::fs::metadata(&file).with_context(|| file.display().to_string())?;
            if meta.len() > MAX_ICON_BYTES as u64 {
                anyhow::bail!("{}: {} bytes; the most is {MAX_ICON_BYTES}", file.display(), meta.len());
            }
            let png = std::fs::read(&file).with_context(|| file.display().to_string())?;
            let (id, new) = icons::add_icon(pool, &name, &png, actor).await?;
            if new {
                store::audit(pool, actor, "icon.add", None, json!({"name": name, "id": id, "bytes": png.len()}))
                    .await?;
                println!("icon {name:?} added: {} ({} bytes)", icon_path(&id), png.len());
            } else {
                println!("icon {name:?} is already this file: {}", icon_path(&id));
            }
        }
        IconCmd::List => {
            let rows: Vec<IconRow> = icons::list_icons(pool).await?;
            if rows.is_empty() {
                println!("no icons (pp-admin icon add <file.png>)");
            }
            for r in rows {
                println!(
                    "{:<20} {:>3}x{:<3} {:>6} B  {}  badges {:<4} main icons {}",
                    r.name, r.width, r.height, r.bytes, r.id, r.badges, r.main_icons
                );
            }
            println!("(at most {MAX_ICON_BYTES} bytes and {MAX_ICON_SIDE}x{MAX_ICON_SIDE} pixels a file)");
        }
        IconCmd::Remove { name, force } => {
            let (id, name) = icons::find_icon(pool, &name).await?;
            let users = icons::remove_icon(pool, &id, force).await?;
            store::audit(pool, actor, "icon.remove", None, json!({"name": name, "id": id, "players": users.len()}))
                .await?;
            println!(
                "icon {name:?} removed{}",
                if users.is_empty() { String::new() } else { format!(", taken off {} player(s)", users.len()) }
            );
        }
    }
    Ok(())
}

pub async fn badge(pool: &PgPool, actor: &str, cmd: BadgeCmd) -> anyhow::Result<()> {
    match cmd {
        BadgeCmd::Add { ident, icon, label, places, pos } => {
            let u = crate::find(pool, &ident).await?;
            let (id, name) = icons::find_icon(pool, &icon).await?;
            let places: Option<Option<Vec<Place>>> =
                places.map(|p| parse_places(&p)).transpose().map_err(|e| anyhow::anyhow!(e))?;
            if pos == Some(0) {
                anyhow::bail!("--pos starts at 1");
            }
            icons::set_badge(pool, u.uid, &id, label.as_deref(), places.clone(), pos).await?;
            store::audit(
                pool,
                actor,
                "badge.set",
                Some(&u.uid.to_string()),
                json!({"icon": name, "label": label, "places": places.map(|p| p.map(|p| p.iter().map(|p| p.as_str()).collect::<Vec<_>>())), "pos": pos}),
            )
            .await?;
            println!("{} has the badge {name:?}", who(&u));
            print_badges(pool, &u).await?;
        }
        BadgeCmd::List { ident } => {
            let u = crate::find(pool, &ident).await?;
            print_badges(pool, &u).await?;
        }
        BadgeCmd::Remove { ident, icon } => {
            let u = crate::find(pool, &ident).await?;
            let (id, name) = icons::find_icon(pool, &icon).await?;
            if icons::remove_badge(pool, u.uid, &id).await? {
                store::audit(pool, actor, "badge.remove", Some(&u.uid.to_string()), json!({"icon": name})).await?;
                println!("{} no longer has the badge {name:?}", who(&u));
            } else {
                println!("{} doesn't have the badge {name:?}", who(&u));
            }
        }
    }
    Ok(())
}

async fn print_badges(pool: &PgPool, u: &UserRow) -> anyhow::Result<()> {
    let badges = icons::badges_of(pool, u.uid).await?;
    if badges.is_empty() {
        println!("{}: no badges", who(u));
    }
    for b in badges {
        let label = if b.label.is_empty() { String::new() } else { format!(" {:?}", b.label) };
        println!("  {}. {}{label} ({})", b.pos, b.name, places_text(&b.places));
    }
    match icons::main_icon_of(pool, u.uid).await? {
        Some((_, name, label)) if label.is_empty() => println!("  main icon: {name}"),
        Some((_, name, label)) => println!("  main icon: {name} {label:?}"),
        None => println!("  main icon: none (their primary main's stock icon)"),
    }
    Ok(())
}

pub async fn main_icon(pool: &PgPool, actor: &str, cmd: MainIconCmd) -> anyhow::Result<()> {
    match cmd {
        MainIconCmd::Set { ident, icon, label } => {
            let u = crate::find(pool, &ident).await?;
            let (id, name) = icons::find_icon(pool, &icon).await?;
            icons::set_main_icon(pool, u.uid, Some((&id, &label))).await?;
            store::audit(pool, actor, "main_icon.set", Some(&u.uid.to_string()), json!({"icon": name, "label": label}))
                .await?;
            println!(
                "{}'s main icon is {name:?}{}",
                who(&u),
                if label.is_empty() { String::new() } else { format!(" ({label:?} on hover)") }
            );
        }
        MainIconCmd::Clear { ident } => {
            let u = crate::find(pool, &ident).await?;
            icons::set_main_icon(pool, u.uid, None).await?;
            store::audit(pool, actor, "main_icon.clear", Some(&u.uid.to_string()), json!({})).await?;
            println!("{} shows their primary main's stock icon again", who(&u));
        }
    }
    Ok(())
}
