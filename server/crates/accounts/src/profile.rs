//! Player profiles (docs/design/profiles.md): one public [`Profile`] per account, read by the
//! launcher (`GET /v1/profiles/<id>`, its View Profile dialog) and shown as a web page
//! (`GET /profile/<slug>`, `brawlonline.net/profile/sond-810`) that search engines index and link
//! previews (Discord) unfurl. Both are built from [`load`], so a field added to [`Profile`] reaches
//! both; the page's HTML is [`page_html`] here, the launcher's view `profile_view.tsx`.
//!
//! The page's address is the connect code, lower case with `-` for `#` ([`slug`]): short, the name
//! players already share, and it changes only when an admin or the slur filter replaces a code
//! (the old link then 404s, which is what a replaced code wants). A uid, any case or a code with
//! leading zeros redirects (308, permanent) to that address. Accounts without a code (online play never set
//! up) and banned accounts have no profile.
//!
//! The player edits their own (the launcher's profile in edit mode): the name through
//! `POST /v1/me/rename` as before, mains, country and social media accounts through
//! `PUT /v1/me/profile` ([`update`]), checked by `common::profile`.

use std::net::SocketAddr;

use axum::extract::{ConnectInfo, Path, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Redirect, Response};
use axum::Json;
use common::codes::{parse_connect_code, ConnectCode};
use common::countries::country_name;
use common::net::ip_key;
use common::profile::{character_name, check_country, check_mains, Platform, ProfileError};
use common::ranked::{self, Rank, Tier};
use serde::{Deserialize, Serialize};
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{client_ip, current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::pages::esc;
use crate::ranked::RankView;
use crate::store::{self, UserRow};
use crate::AppState;

/// A player's public profile.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Profile {
    pub uid: Uuid,
    pub display_name: String,
    /// `ABCD#123`; empty before online play is set up.
    pub connect_code: String,
    /// The profile's web page (`<PUBLIC_BASE_URL>/profile/<slug>`); null without a connect code.
    pub link: Option<String>,
    pub ranked: ProfileRanked,
    /// Character keys (`common::profile::CHARACTERS`), the primary first; empty for none.
    pub mains: Vec<String>,
    /// A flag-icons code (`se`, `gb-eng`); null for none.
    pub country: Option<String>,
    /// The accounts set, in [`Platform::ALL`]'s order.
    pub socials: Vec<Social>,
    /// The player shows their flag beside their name around the launcher (the leaderboard, the
    /// online list, rooms, chat), not only here.
    pub show_country: bool,
    /// The same for their primary main (the leaderboard, chat).
    pub show_main: bool,
}

/// What shows beside a player's name in the launcher's lists ([`name_tags`]): the flag and the
/// primary main, each only when set and the player chose to show it.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NameTag {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub country: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main: Option<String>,
}

impl NameTag {
    pub fn is_empty(&self) -> bool {
        self.country.is_none() && self.main.is_none()
    }
}

/// Each player's [`NameTag`] (players with nothing to show are left out).
pub async fn name_tags<'e, E: sqlx::PgExecutor<'e>>(
    e: E,
    uids: &[Uuid],
) -> sqlx::Result<std::collections::HashMap<Uuid, NameTag>> {
    if uids.is_empty() {
        return Ok(Default::default());
    }
    let rows: Vec<(Uuid, Option<String>, Option<String>)> = sqlx::query_as(
        "SELECT uid, CASE WHEN show_country THEN country END, CASE WHEN show_main THEN mains[1] END
           FROM profiles WHERE uid = ANY($1) AND ((show_country AND country IS NOT NULL) OR (show_main AND cardinality(mains) > 0))",
    )
    .bind(uids)
    .fetch_all(e)
    .await?;
    Ok(rows
        .into_iter()
        .map(|(uid, country, main)| {
            let tag = NameTag {
                country: country.filter(|c| country_name(c).is_some()),
                main: main.filter(|m| character_name(m).is_some()),
            };
            (uid, tag)
        })
        .filter(|(_, t)| !t.is_empty())
        .collect())
}

/// A social media account on a profile.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Social {
    /// `discord`, `twitter`, `twitch`, `youtube`, `tiktok` or `instagram`.
    pub platform: &'static str,
    /// As stored: `sarahkitty`, YouTube's `@name` or `channel/UC...`.
    pub handle: String,
    /// As shown: `@sarahkitty`, Discord's as it is.
    pub display: String,
    /// The account's page; null for Discord (a username, not a page).
    pub url: Option<String>,
}

impl Social {
    fn of(platform: Platform, handle: String) -> Social {
        Social { platform: platform.key(), display: platform.display(&handle), url: platform.url(&handle), handle }
    }
}

/// A `profiles` row (an empty profile without one): mains, country, each platform's handle in
/// [`Platform::ALL`]'s order, and the two "show next to name" choices (on until turned off).
#[derive(Debug, Clone)]
struct StoredProfile {
    mains: Vec<String>,
    country: Option<String>,
    handles: [Option<String>; 6],
    show_country: bool,
    show_main: bool,
}

impl Default for StoredProfile {
    fn default() -> Self {
        StoredProfile { mains: vec![], country: None, handles: Default::default(), show_country: true, show_main: true }
    }
}

type RawProfileRow = (
    Vec<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
    bool,
    bool,
);

async fn profile_row(pool: &PgPool, uid: Uuid) -> sqlx::Result<StoredProfile> {
    let row: Option<RawProfileRow> = sqlx::query_as(
        "SELECT mains, country, discord, twitter, twitch, youtube, tiktok, instagram, show_country, show_main
           FROM profiles WHERE uid = $1",
    )
    .bind(uid)
    .fetch_optional(pool)
    .await?;
    Ok(row.map_or_else(StoredProfile::default, |(mains, country, a, b, t, y, k, i, show_country, show_main)| {
        StoredProfile { mains, country, handles: [a, b, t, y, k, i], show_country, show_main }
    }))
}

/// The player's Ranked standing, as the leaderboard and Home show it.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileRanked {
    /// The rank held (docs/design/ranked-tiers.md); null while placing.
    pub rank: Option<RankView>,
    /// The Elo rating; null while placing (nobody sees it until then, `????` in the launcher).
    pub rating: Option<f64>,
    /// Rated sets played.
    pub sets_played: u32,
    /// Rated sets left before the player places; 0 once placed.
    pub sets_to_place: u32,
    /// Ranked sets won and lost, placement sets included.
    pub wins: u32,
    pub losses: u32,
    /// 1-based leaderboard position; null while placing.
    pub position: Option<u64>,
    /// Players on the leaderboard.
    pub ranked_players: u64,
}

/// The page address of a code: `SOND#810` is `sond-810`.
pub fn slug(code: &str) -> String {
    code.replace('#', "-").to_lowercase()
}

/// Who an address names: a uid, or a code as [`slug`] writes it (any case; `ABCD#12` too).
#[derive(Debug, Clone, PartialEq)]
pub enum Ident {
    Uid(Uuid),
    Code(ConnectCode),
}

impl Ident {
    pub fn parse(s: &str) -> Option<Ident> {
        let s = s.trim();
        if let Ok(uid) = Uuid::parse_str(s) {
            return Some(Ident::Uid(uid));
        }
        // The prefix is letters only, so the last '-' is the code's '#'.
        let code = match s.rsplit_once('-') {
            Some((prefix, number)) if !s.contains('#') => format!("{prefix}#{number}"),
            _ => s.to_string(),
        };
        parse_connect_code(&code).ok().map(Ident::Code)
    }
}

/// The account an address names and its profile; `None` for nobody, a banned account, and (by
/// code) an account whose code it no longer is.
pub async fn load(pool: &PgPool, ident: &Ident, base_url: &str) -> sqlx::Result<Option<Profile>> {
    let user = match ident {
        Ident::Uid(uid) => store::user_by_uid(pool, *uid).await?,
        Ident::Code(code) => store::find_user(pool, &code.to_string()).await?,
    };
    let Some(user) = user.filter(|u| !u.is_banned()) else {
        return Ok(None);
    };
    Ok(Some(profile_of(pool, &user, base_url).await?))
}

async fn profile_of(pool: &PgPool, user: &UserRow, base_url: &str) -> sqlx::Result<Profile> {
    let standing = crate::ranked::standing(pool, user.uid).await?;
    let (position, ranked_players) = crate::leaderboard::placement(pool, user.uid, standing).await?;
    let tier = crate::ranked::tier_state(pool, user.uid).await?;
    let (wins, losses) = crate::ranked::record(pool, user.uid).await?;
    let placed = ranked::placed(standing.sets_played);
    let code = user.connect_code.clone().unwrap_or_default();
    let stored = profile_row(pool, user.uid).await?;
    // Only what the lists still know (a character or flag dropped from them since is left out).
    let mains = stored.mains.into_iter().filter(|m| character_name(m).is_some()).collect();
    let country = stored.country.filter(|c| country_name(c).is_some());
    let socials = Platform::ALL.into_iter().zip(stored.handles).filter_map(|(p, h)| Some(Social::of(p, h?))).collect();
    Ok(Profile {
        uid: user.uid,
        display_name: user.display_name.clone(),
        link: (!code.is_empty()).then(|| format!("{}/profile/{}", base_url.trim_end_matches('/'), slug(&code))),
        connect_code: code,
        ranked: ProfileRanked {
            rank: RankView::of(tier.rank.filter(|_| placed)),
            rating: placed.then_some(standing.rating),
            sets_played: standing.sets_played,
            sets_to_place: ranked::sets_to_place(standing.sets_played),
            wins,
            losses,
            position,
            ranked_players,
        },
        mains,
        country,
        socials,
        show_country: stored.show_country,
        show_main: stored.show_main,
    })
}

/// `PUT /v1/me/profile`: changes the player's mains, country, social media accounts and whether the
/// flag and primary main show beside their name (`showCountry`, `showMain`). Each field
/// sent replaces what is stored; one left out stays. `mains`: character keys, the primary first
/// (at most [`common::profile::MAX_MAINS`], no repeats); `country`: a flag code or null;
/// `socials`: `{platform: text}`, where the text is a username, `@username` or a link to the
/// account (stored as its handle; blank or null removes it). A username the slur filter calls a
/// slur is refused. 400 for anything else not allowed, with a message for the player. Session
/// auth; limited per account. Answers the profile as `GET /v1/profiles/<uid>` does.
pub async fn update(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(req): Json<ProfileUpdate>,
) -> ApiResult<Json<Profile>> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.profile_edit.check(&user.uid, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many changes. Try again in {} s.", wait.as_secs().max(1)))
    })?;
    let bad = |e: ProfileError| ApiError::bad_request("invalid_profile", e.to_string());
    let mut stored = profile_row(&state.pool, user.uid).await?;
    if let Some(m) = req.mains {
        check_mains(&m).map_err(bad)?;
        stored.mains = m;
    }
    if let Some(c) = req.country {
        stored.country = match c.as_deref().map(str::trim).filter(|c| !c.is_empty()) {
            Some(code) => Some(check_country(code).map_err(bad)?),
            None => None,
        };
    }
    if let Some(v) = req.show_country {
        stored.show_country = v;
    }
    if let Some(v) = req.show_main {
        stored.show_main = v;
    }
    let handles = &mut stored.handles;
    if let Some(socials) = req.socials {
        for (key, text) in socials {
            let platform = Platform::from_key(&key)
                .ok_or_else(|| ApiError::bad_request("invalid_profile", format!("Unknown platform: {key}")))?;
            let i = Platform::ALL.iter().position(|p| *p == platform).unwrap_or(0);
            let handle = platform.parse(text.as_deref().unwrap_or("")).map_err(bad)?;
            if let Some(h) = &handle {
                if handles[i].as_deref() != Some(h.as_str()) {
                    match state.slur_filter.is_slur(crate::slur_filter::What::SocialHandle, h).await {
                        Ok(true) => {
                            return Err(ApiError::bad_request(
                                "invalid_profile",
                                format!("That {} username isn't allowed.", platform.name()),
                            ))
                        }
                        Ok(false) => {}
                        // Taken as typed when the model doesn't answer (the admins can clear it).
                        Err(e) => tracing::warn!("slur check of a social handle failed: {e:#}"),
                    }
                }
            }
            handles[i] = handle;
        }
    }
    let [discord, twitter, twitch, youtube, tiktok, instagram] = stored.handles.clone();
    sqlx::query(
        "INSERT INTO profiles (uid, mains, country, discord, twitter, twitch, youtube, tiktok, instagram,
                               show_country, show_main, updated_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, now())
         ON CONFLICT (uid) DO UPDATE SET mains = $2, country = $3, discord = $4, twitter = $5, twitch = $6,
                                         youtube = $7, tiktok = $8, instagram = $9, show_country = $10,
                                         show_main = $11, updated_at = now()",
    )
    .bind(user.uid)
    .bind(&stored.mains)
    .bind(&stored.country)
    .bind(discord)
    .bind(twitter)
    .bind(twitch)
    .bind(youtube)
    .bind(tiktok)
    .bind(instagram)
    .bind(stored.show_country)
    .bind(stored.show_main)
    .execute(&state.pool)
    .await?;
    // The online list and chat show the new flag and main at once.
    let tag = name_tags(&state.pool, &[user.uid]).await?.remove(&user.uid).unwrap_or_default();
    state.social.name_tag_changed(&user.uid.to_string(), tag);
    Ok(Json(profile_of(&state.pool, &user, &state.cfg.public_base_url).await?))
}

/// The body of `PUT /v1/me/profile`. `country: null` removes the country; a missing field is left
/// as it is (`Option<Option<_>>` tells the two apart).
#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProfileUpdate {
    pub mains: Option<Vec<String>>,
    #[serde(default, deserialize_with = "present")]
    pub country: Option<Option<String>>,
    pub socials: Option<std::collections::BTreeMap<String, Option<String>>>,
    pub show_country: Option<bool>,
    pub show_main: Option<bool>,
}

/// A field that is present, even as null, is `Some`.
fn present<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<Option<String>>, D::Error> {
    Option::<String>::deserialize(d).map(Some)
}

fn check_limit(state: &AppState, headers: &HeaderMap, peer: SocketAddr) -> ApiResult<()> {
    let ip = ip_key(client_ip(state, headers, peer));
    limit(state, |l, now| l.profile_ip.check(&ip, now)).map_err(|wait| {
        ApiError::rate_limited(wait)
            .with_message(format!("Too many requests. Try again in {} s.", wait.as_secs().max(1)))
    })
}

/// `GET /v1/profiles/<id>`: a profile by uid or connect code (`ABCD#123` encoded, or `abcd-123`).
/// Public; limited per client IP together with the page ([`common::ratelimit::PROFILE_IP_WINDOWS`]).
/// 404 for nobody and for a banned account.
pub async fn lookup(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Json<Profile>> {
    check_limit(&state, &headers, peer)?;
    let ident = Ident::parse(&id).ok_or_else(ApiError::not_found)?;
    let profile = load(&state.pool, &ident, &state.cfg.public_base_url).await?.ok_or_else(ApiError::not_found)?;
    Ok(Json(profile))
}

/// `GET /profile/<slug>`: the profile's web page, the same HTML for people, search engines and link
/// previews (none of it needs a script). Another spelling of the address redirects to
/// [`slug`]'s; an unknown one is a 404 page.
pub async fn page(
    State(state): State<AppState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    if let Err(e) = check_limit(&state, &headers, peer) {
        return e.into_response();
    }
    let base = state.cfg.public_base_url.trim_end_matches('/').to_string();
    let found = match Ident::parse(&id) {
        Some(ident) => load(&state.pool, &ident, &base).await,
        None => Ok(None),
    };
    match found {
        Ok(Some(p)) if !p.connect_code.is_empty() => {
            let canonical = slug(&p.connect_code);
            if id != canonical {
                return Redirect::permanent(&format!("/profile/{canonical}")).into_response();
            }
            html_response(StatusCode::OK, page_html(&p, &base))
        }
        Ok(_) => html_response(StatusCode::NOT_FOUND, not_found_html()),
        Err(e) => {
            tracing::error!("profile page: {e}");
            html_response(StatusCode::INTERNAL_SERVER_ERROR, error_html())
        }
    }
}

fn html_response(status: StatusCode, body: String) -> Response {
    let mut resp = (status, body).into_response();
    let h = resp.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("text/html; charset=utf-8"));
    // No scripts at all; styles, the font and the badge images from the site itself.
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static(
            "default-src 'none'; style-src 'self'; font-src 'self'; img-src 'self'; base-uri 'none'; \
             form-action 'none'; frame-ancestors 'none'",
        ),
    );
    h.insert(header::REFERRER_POLICY, HeaderValue::from_static("no-referrer"));
    if status == StatusCode::OK {
        // A minute: the rating changes after each set.
        h.insert(header::CACHE_CONTROL, HeaderValue::from_static("public, max-age=60"));
    }
    resp
}

/// A rank's name: "Gold 2", "Legendary".
pub fn rank_name(rank: &RankView) -> String {
    let tier = tier_name(rank.tier);
    match rank.division {
        Some(d) => format!("{tier} {d}"),
        None => tier.to_string(),
    }
}

fn tier_name(tier: &str) -> &'static str {
    match tier {
        "bronze" => "Bronze",
        "silver" => "Silver",
        "gold" => "Gold",
        "emerald" => "Emerald",
        "diamond" => "Diamond",
        "master" => "Master",
        _ => "Legendary",
    }
}

/// The badge image on the website (`website/assets/ranks/`).
fn badge_path(rank: &RankView) -> String {
    match rank.division {
        Some(d) => format!("/assets/ranks/{}-{d}.png", rank.tier),
        None => format!("/assets/ranks/{}.png", rank.tier),
    }
}

/// The link preview's side bar (Discord's embed colour): the tier's colour, else the site's.
fn theme_color(rank: Option<&RankView>) -> &'static str {
    let tier = Tier::ALL.into_iter().find(|t| rank.is_some_and(|r| r.tier == t.as_str()));
    match tier {
        Some(Tier::Bronze) => "#c27a48",
        Some(Tier::Silver) => "#b9c3cf",
        Some(Tier::Gold) => "#e9b52f",
        Some(Tier::Emerald) => "#2fbf71",
        Some(Tier::Diamond) => "#5aa9ec",
        Some(Tier::Master) => "#a46be6",
        Some(Tier::Legendary) => "#ef5a3c",
        None => "#74de55",
    }
}

fn rating_text(r: f64) -> String {
    format!("{r:.1}")
}

/// The rank line under the name: "Gold 2", "Placing: 3 more ranked sets", "Not ranked yet".
fn standing_text(r: &ProfileRanked) -> String {
    if let Some(rank) = &r.rank {
        rank_name(rank)
    } else if r.sets_played > 0 && r.sets_to_place > 0 {
        let n = r.sets_to_place;
        format!("Placing: {n} more ranked set{}", if n == 1 { "" } else { "s" })
    } else {
        "Not ranked yet".to_string()
    }
}

/// The search snippet and link preview text: "Gold 2 · Rating 1523.4 · #12 of 340 · 34W-20L";
/// without a rank "Placing. 1W-1L" (Sarah's, 2026-11-09).
pub fn summary(p: &Profile) -> String {
    let r = &p.ranked;
    let record = format!("{}W-{}L", r.wins, r.losses);
    let Some(rank) = &r.rank else {
        return format!("Placing. {record}");
    };
    let mut parts = vec![rank_name(rank)];
    if let Some(rating) = r.rating {
        parts.push(format!("Rating {}", rating_text(rating)));
    }
    if let Some(pos) = r.position {
        parts.push(format!("#{pos} of {}", r.ranked_players));
    }
    parts.push(record);
    parts.join(" · ")
}

/// The emblems' common silhouette (as the launcher's and the top 10's), drawn dotted while placing.
const RANK_OUTLINE: &str = "M123 9 L112 16 L94 34 L49 35 L32 39 L27 46 L26 64 L17 69 L16 81 L22 116 L27 121 L30 \
    174 L36 191 L52 207 L75 223 L116 247 L131 249 L142 245 L194 213 L209 201 L220 188 L225 165 L227 124 L236 116 \
    L240 97 L242 72 L238 66 L228 65 L227 46 L222 39 L203 35 L159 34 L136 12 L130 9 Z";

fn badge_html(r: &ProfileRanked) -> String {
    if let Some(rank) = &r.rank {
        let name = esc(&rank_name(rank));
        return format!(
            "<img class=\"emblem\" src=\"{}\" alt=\"{name}\" title=\"{name}\" width=\"256\" height=\"256\">",
            badge_path(rank)
        );
    }
    if r.sets_played > 0 && r.sets_to_place > 0 {
        return format!(
            "<svg class=\"emblem placing\" viewBox=\"0 0 256 256\" aria-hidden=\"true\"><path d=\"{RANK_OUTLINE}\"/></svg>\
             <span class=\"more\">{} more</span>",
            r.sets_to_place
        );
    }
    String::new()
}

fn stat(label: &str, value: &str) -> String {
    format!("<div><dt>{}</dt><dd>{}</dd></div>", esc(label), esc(value))
}

/// Text for a `<script type="application/ld+json">`: JSON with `<` escaped, so a name can't end
/// the element.
fn json_ld(v: &serde_json::Value) -> String {
    v.to_string().replace('<', "\\u003c").replace('>', "\\u003e").replace('&', "\\u0026")
}

/// The profile page. `base` is the public base URL (absolute links for previews and search).
pub fn page_html(p: &Profile, base: &str) -> String {
    let r = &p.ranked;
    let name = esc(&p.display_name);
    let code = esc(&p.connect_code);
    let url = esc(&format!("{base}/profile/{}", slug(&p.connect_code)));
    let title = format!("{} ({})", p.display_name, p.connect_code);
    let summary = summary(p);
    let image = match &r.rank {
        Some(rank) => format!("{base}{}", badge_path(rank)),
        // The emblems' outline, dotted, with no "N more" tag (website/assets/placing-badge.png).
        None => format!("{base}/assets/placing-badge.png"),
    };
    let ld = json_ld(&serde_json::json!({
        "@context": "https://schema.org",
        "@type": "ProfilePage",
        "url": format!("{base}/profile/{}", slug(&p.connect_code)),
        "mainEntity": {
            "@type": "Person",
            "name": p.display_name,
            "alternateName": p.connect_code,
            "identifier": p.uid.to_string(),
        },
    }));

    let mut stats = String::new();
    match r.rating {
        Some(rating) => stats.push_str(&stat("Rating", &rating_text(rating))),
        None if r.sets_played > 0 => stats.push_str(&stat("Rating", "????")),
        None => {}
    }
    if let Some(pos) = r.position {
        stats.push_str(&stat("Leaderboard", &format!("#{pos} of {}", r.ranked_players)));
    }
    stats.push_str(&stat("Sets played", &r.sets_played.to_string()));
    if r.sets_played > 0 {
        stats.push_str(&stat("Record", &format!("{}W-{}L", r.wins, r.losses)));
    }

    format!(
        "<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">
<meta name=\"color-scheme\" content=\"dark\">
<meta name=\"theme-color\" content=\"{theme}\">
<title>{title} · Brawl Online</title>
<meta name=\"description\" content=\"{name}'s Brawl Online profile: {summary}.\">
<link rel=\"canonical\" href=\"{url}\">
<meta property=\"og:site_name\" content=\"Brawl Online\">
<meta property=\"og:type\" content=\"profile\">
<meta property=\"og:title\" content=\"{title}\">
<meta property=\"og:description\" content=\"{summary}\">
<meta property=\"og:url\" content=\"{url}\">
<meta property=\"og:image\" content=\"{image}\">
<meta property=\"profile:username\" content=\"{name}\">
<meta name=\"twitter:card\" content=\"summary\">
<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"16x16 32x32 48x48\">
<link rel=\"icon\" href=\"/assets/icon-512.png\" type=\"image/png\" sizes=\"512x512\">
<link rel=\"preload\" href=\"/assets/fonts/barlow-condensed-900-italic.woff2\" as=\"font\" type=\"font/woff2\" crossorigin>
<link rel=\"stylesheet\" href=\"/profile/profile.css\">
<script type=\"application/ld+json\">{ld}</script>
</head>
<body>
<header>
  <a class=\"home\" href=\"/\" aria-label=\"Brawl Online home\"><span class=\"w1\">Brawl</span> <span class=\"w2\">Online</span></a>
</header>
<main>
  <section class=\"card who\" data-tier=\"{tier}\">
    <div class=\"badge\">{badge}</div>
    <div>
      <h1 class=\"name\">{name}</h1>
      <p class=\"code\">{code}</p>
{country}
      <p class=\"standing\">{standing}</p>
    </div>
  </section>
{about}  <section class=\"card\">
    <h2>Ranked</h2>
    <dl class=\"stats\">{stats}</dl>
  </section>
{socials}  <p class=\"cta\">Wii/GCN games with rollback netcode. <a href=\"/\">Download Brawl Online</a></p>
</main>
</body>
</html>
",
        theme = theme_color(r.rank.as_ref()),
        title = esc(&title),
        summary = esc(&summary),
        image = esc(&image),
        tier = r.rank.as_ref().map_or("none", |k| k.tier),
        badge = badge_html(r),
        standing = esc(&standing_text(r)),
        about = about_html(p),
        country = country_html(p),
        socials = socials_html(p),
    )
}

/// The flag and the country's name, under the code (as in the launcher); nothing without one.
fn country_html(p: &Profile) -> String {
    match p.country.as_deref().and_then(|c| Some((c, country_name(c)?))) {
        Some((code, name)) => format!(
            "      <p class=\"country\"><img class=\"flag\" src=\"/assets/flags/{code}.svg\" alt=\"\" width=\"20\" \
             height=\"15\">{}</p>\n",
            esc(name)
        ),
        None => String::new(),
    }
}

/// Mains (each a name tag; the website has no stock icons, they come from the player's own disc);
/// nothing without any.
fn about_html(p: &Profile) -> String {
    let mut rows = String::new();
    if !p.mains.is_empty() {
        let tags: String = p
            .mains
            .iter()
            .enumerate()
            .filter_map(|(i, m)| {
                let name = esc(character_name(m)?);
                Some(if i == 0 {
                    format!("<li class=\"main primary\" title=\"Primary\">{name}</li>")
                } else {
                    format!("<li class=\"main\">{name}</li>")
                })
            })
            .collect();
        rows.push_str(&format!(
            "<div class=\"row\"><span class=\"label\">Mains:</span><ul class=\"mains\">{tags}</ul></div>"
        ));
    }
    if rows.is_empty() {
        return String::new();
    }
    format!("  <section class=\"card about\">{rows}</section>\n")
}

/// The social media row: each account's logo linking to it (Discord's with the username beside it,
/// as it has no page); nothing without any.
fn socials_html(p: &Profile) -> String {
    if p.socials.is_empty() {
        return String::new();
    }
    let items: String = p
        .socials
        .iter()
        .filter_map(|s| {
            let platform = Platform::from_key(s.platform)?;
            let icon = format!(
                "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"{}\"/></svg>",
                social_icon_path(platform)
            );
            let label = esc(&format!("{}: {}", platform.name(), s.display));
            Some(match &s.url {
                Some(url) => format!(
                    "<li><a href=\"{}\" title=\"{label}\" aria-label=\"{label}\" rel=\"me nofollow noopener\">{icon}</a></li>",
                    esc(url)
                ),
                None => format!("<li class=\"handle\" title=\"{label}\">{icon}<span>{}</span></li>", esc(&s.display)),
            })
        })
        .collect();
    format!("  <section class=\"card\">\n    <h2>Socials</h2>\n    <ul class=\"socials\">{items}</ul>\n  </section>\n")
}

/// Each platform's logo, a 24x24 SVG path (simple-icons 16.37.0, CC0; X for Twitter). The launcher has the same
/// paths (`launcher/src/renderer/components/profile/social_icons.ts`).
fn social_icon_path(p: Platform) -> &'static str {
    match p {
        Platform::Discord => "M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z",
        Platform::Twitter => "M14.234 10.162 22.977 0h-2.072l-7.591 8.824L7.251 0H.258l9.168 13.343L.258 24H2.33l8.016-9.318L16.749 24h6.993zm-2.837 3.299-.929-1.329L3.076 1.56h3.182l5.965 8.532.929 1.329 7.754 11.09h-3.182z",
        Platform::Twitch => "M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143l-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714Z",
        Platform::Youtube => "M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432L15.818 12l-6.273 3.568z",
        Platform::Tiktok => "M12.525.02c1.31-.02 2.61-.01 3.91-.02.08 1.53.63 3.09 1.75 4.17 1.12 1.11 2.7 1.62 4.24 1.79v4.03c-1.44-.05-2.89-.35-4.2-.97-.57-.26-1.1-.59-1.62-.93-.01 2.92.01 5.84-.02 8.75-.08 1.4-.54 2.79-1.35 3.94-1.31 1.92-3.58 3.17-5.91 3.21-1.43.08-2.86-.31-4.08-1.03-2.02-1.19-3.44-3.37-3.65-5.71-.02-.5-.03-1-.01-1.49.18-1.9 1.12-3.72 2.58-4.96 1.66-1.44 3.98-2.13 6.15-1.72.02 1.48-.04 2.96-.04 4.44-.99-.32-2.15-.23-3.02.37-.63.41-1.11 1.04-1.36 1.75-.21.51-.15 1.07-.14 1.61.24 1.64 1.82 3.02 3.5 2.87 1.12-.01 2.19-.66 2.77-1.61.19-.33.4-.67.41-1.06.1-1.79.06-3.57.07-5.36.01-4.03-.01-8.05.02-12.07z",
        Platform::Instagram => "M7.0301.084c-1.2768.0602-2.1487.264-2.911.5634-.7888.3075-1.4575.72-2.1228 1.3877-.6652.6677-1.075 1.3368-1.3802 2.127-.2954.7638-.4956 1.6365-.552 2.914-.0564 1.2775-.0689 1.6882-.0626 4.947.0062 3.2586.0206 3.6671.0825 4.9473.061 1.2765.264 2.1482.5635 2.9107.308.7889.72 1.4573 1.388 2.1228.6679.6655 1.3365 1.0743 2.1285 1.38.7632.295 1.6361.4961 2.9134.552 1.2773.056 1.6884.069 4.9462.0627 3.2578-.0062 3.668-.0207 4.9478-.0814 1.28-.0607 2.147-.2652 2.9098-.5633.7889-.3086 1.4578-.72 2.1228-1.3881.665-.6682 1.0745-1.3378 1.3795-2.1284.2957-.7632.4966-1.636.552-2.9124.056-1.2809.0692-1.6898.063-4.948-.0063-3.2583-.021-3.6668-.0817-4.9465-.0607-1.2797-.264-2.1487-.5633-2.9117-.3084-.7889-.72-1.4568-1.3876-2.1228C21.2982 1.33 20.628.9208 19.8378.6165 19.074.321 18.2017.1197 16.9244.0645 15.6471.0093 15.236-.005 11.977.0014 8.718.0076 8.31.0215 7.0301.0839m.1402 21.6932c-1.17-.0509-1.8053-.2453-2.2287-.408-.5606-.216-.96-.4771-1.3819-.895-.422-.4178-.6811-.8186-.9-1.378-.1644-.4234-.3624-1.058-.4171-2.228-.0595-1.2645-.072-1.6442-.079-4.848-.007-3.2037.0053-3.583.0607-4.848.05-1.169.2456-1.805.408-2.2282.216-.5613.4762-.96.895-1.3816.4188-.4217.8184-.6814 1.3783-.9003.423-.1651 1.0575-.3614 2.227-.4171 1.2655-.06 1.6447-.072 4.848-.079 3.2033-.007 3.5835.005 4.8495.0608 1.169.0508 1.8053.2445 2.228.408.5608.216.96.4754 1.3816.895.4217.4194.6816.8176.9005 1.3787.1653.4217.3617 1.056.4169 2.2263.0602 1.2655.0739 1.645.0796 4.848.0058 3.203-.0055 3.5834-.061 4.848-.051 1.17-.245 1.8055-.408 2.2294-.216.5604-.4763.96-.8954 1.3814-.419.4215-.8181.6811-1.3783.9-.4224.1649-1.0577.3617-2.2262.4174-1.2656.0595-1.6448.072-4.8493.079-3.2045.007-3.5825-.006-4.848-.0608M16.953 5.5864A1.44 1.44 0 1 0 18.39 4.144a1.44 1.44 0 0 0-1.437 1.4424M5.8385 12.012c.0067 3.4032 2.7706 6.1557 6.173 6.1493 3.4026-.0065 6.157-2.7701 6.1506-6.1733-.0065-3.4032-2.771-6.1565-6.174-6.1498-3.403.0067-6.156 2.771-6.1496 6.1738M8 12.0077a4 4 0 1 1 4.008 3.9921A3.9996 3.9996 0 0 1 8 12.0077",
    }
}

fn plain_page(title: &str, text: &str) -> String {
    format!(
        "<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<meta name=\"color-scheme\" content=\"dark\">
<meta name=\"robots\" content=\"noindex\">
<title>{title} · Brawl Online</title>
<link rel=\"icon\" href=\"/favicon.ico\" sizes=\"16x16 32x32 48x48\">
<link rel=\"stylesheet\" href=\"/profile/profile.css\">
</head>
<body>
<header>
  <a class=\"home\" href=\"/\" aria-label=\"Brawl Online home\"><span class=\"w1\">Brawl</span> <span class=\"w2\">Online</span></a>
</header>
<main>
  <section class=\"card\"><h1 class=\"name\">{title}</h1><p class=\"standing\">{text}</p></section>
</main>
</body>
</html>
",
        title = esc(title),
        text = esc(text),
    )
}

fn not_found_html() -> String {
    plain_page("No such player", "No player has this code. Codes look like ABCD#123: the address is /profile/abcd-123.")
}

fn error_html() -> String {
    plain_page("Something went wrong", "Try again in a minute.")
}

/// [`RankView`] of a stored rank index (`rating_events.rank_after`), for the match history.
pub fn rank_view_of_index(i: Option<i16>) -> Option<RankView> {
    RankView::of(i.and_then(Rank::from_index))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile(rank: Option<RankView>, rating: Option<f64>, sets: u32) -> Profile {
        Profile {
            uid: Uuid::from_u128(7),
            display_name: "Sonder<b>\"&".into(),
            connect_code: "SOND#810".into(),
            link: Some("https://brawlonline.net/profile/sond-810".into()),
            ranked: ProfileRanked {
                rank,
                rating,
                sets_played: sets,
                sets_to_place: ranked::sets_to_place(sets),
                wins: sets / 2,
                losses: sets - sets / 2,
                position: rating.map(|_| 12),
                ranked_players: 340,
            },
            mains: vec![],
            country: None,
            socials: vec![],
            show_country: true,
            show_main: true,
        }
    }

    #[test]
    fn addresses() {
        assert_eq!(slug("SOND#810"), "sond-810");
        let code =
            |s: &str| Ident::parse(s).and_then(|i| if let Ident::Code(c) = i { Some(c.to_string()) } else { None });
        assert_eq!(code("sond-810").as_deref(), Some("SOND#810"));
        assert_eq!(code("SOND-0810").as_deref(), Some("SOND#810"));
        assert_eq!(code("SOND#810").as_deref(), Some("SOND#810"));
        assert_eq!(code("abc-1").as_deref(), Some("ABC#1"));
        assert_eq!(code("sond"), None);
        assert_eq!(code("so-nd-810"), None);
        assert_eq!(code("sond-"), None);
        assert_eq!(code("sond-81x"), None);
        let uid = Uuid::from_u128(42);
        assert_eq!(Ident::parse(&uid.to_string()), Some(Ident::Uid(uid)));
    }

    #[test]
    fn page_of_a_placed_player() {
        let gold2 = RankView { tier: "gold", division: Some(2) };
        let p = profile(Some(gold2), Some(1523.44), 54);
        assert_eq!(summary(&p), "Gold 2 · Rating 1523.4 · #12 of 340 · 27W-27L");
        let html = page_html(&p, "https://brawlonline.net");
        // The name is escaped everywhere it goes.
        assert!(!html.contains("Sonder<b>"), "{html}");
        assert!(html.contains("<h1 class=\"name\">Sonder&lt;b&gt;&quot;&amp;</h1>"), "{html}");
        assert!(html.contains("\"name\":\"Sonder\\u003cb\\u003e\\\"\\u0026\""), "{html}");
        // What Discord reads.
        assert!(html.contains("<meta property=\"og:title\" content=\"Sonder&lt;b&gt;&quot;&amp; (SOND#810)\">"));
        assert!(html.contains("content=\"Gold 2 · Rating 1523.4 · #12 of 340 · 27W-27L\""));
        assert!(
            html.contains("<meta property=\"og:image\" content=\"https://brawlonline.net/assets/ranks/gold-2.png\">")
        );
        assert!(html.contains("<meta name=\"theme-color\" content=\"#e9b52f\">"));
        assert!(html.contains("<link rel=\"canonical\" href=\"https://brawlonline.net/profile/sond-810\">"));
        assert!(html.contains("<dt>Leaderboard</dt><dd>#12 of 340</dd>"));
        assert!(!html.contains("noindex"));
        let legend = profile(Some(RankView { tier: "legendary", division: None }), Some(2300.0), 80);
        assert!(page_html(&legend, "").contains("src=\"/assets/ranks/legendary.png\""));
    }

    #[test]
    fn page_shows_mains_country_and_socials_when_set() {
        let mut p = profile(None, None, 0);
        let html = page_html(&p, "");
        assert!(!html.contains("Mains") && !html.contains("class=\"country\"") && !html.contains("Socials"), "{html}");
        p.mains = vec!["marth".into(), "random".into()];
        p.country = Some("gb-eng".into());
        p.socials = vec![
            Social::of(Platform::Discord, "sarah<kitty>".into()),
            Social::of(Platform::Twitch, "sarahkitty".into()),
        ];
        let html = page_html(&p, "");
        assert!(
            html.contains("<li class=\"main primary\" title=\"Primary\">Marth</li><li class=\"main\">Random</li>"),
            "{html}"
        );
        assert!(html.contains("src=\"/assets/flags/gb-eng.svg\""));
        // The country under the code; socials in their own section.
        assert!(html.contains(">England</p>"), "{html}");
        assert!(html.find("class=\"code\"").unwrap() < html.find("class=\"country\"").unwrap());
        assert!(html.contains("<h2>Socials</h2>"));
        assert!(html.contains("<a href=\"https://twitch.tv/sarahkitty\" title=\"Twitch: @sarahkitty\""));
        assert!(html.contains("<span>sarah&lt;kitty&gt;</span>"));
    }

    #[test]
    fn page_of_a_placing_and_a_new_player() {
        let p = profile(None, None, 2);
        assert_eq!(summary(&p), "Placing. 1W-1L");
        let html = page_html(&p, "https://brawlonline.net");
        assert!(html.contains("<dt>Rating</dt><dd>????</dd>"), "{html}");
        assert!(html.contains("3 more</span>"));
        assert!(!html.contains("Leaderboard"));
        assert!(html.contains("og:image\" content=\"https://brawlonline.net/assets/placing-badge.png\""));
        let p = profile(None, None, 0);
        assert_eq!(summary(&p), "Placing. 0W-0L");
        let html = page_html(&p, "");
        assert!(!html.contains("Rating") && !html.contains("Record"), "{html}");
        assert!(html.contains("<dt>Sets played</dt><dd>0</dd>"));
    }
}
