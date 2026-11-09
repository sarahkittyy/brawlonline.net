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

use std::net::SocketAddr;

use axum::extract::{ConnectInfo, Path, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Redirect, Response};
use axum::Json;
use common::codes::{parse_connect_code, ConnectCode};
use common::net::ip_key;
use common::ranked::{self, Rank, Tier};
use serde::Serialize;
use sqlx::PgPool;
use uuid::Uuid;

use crate::api::{client_ip, limit};
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
    })
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

/// The search snippet and link preview text: "Gold 2 · Rating 1523.4 · #12 of 340 · 34-20 in
/// Ranked".
pub fn summary(p: &Profile) -> String {
    let r = &p.ranked;
    let mut parts = vec![standing_text(r)];
    if let Some(rating) = r.rating {
        parts.push(format!("Rating {}", rating_text(rating)));
    }
    if let Some(pos) = r.position {
        parts.push(format!("#{pos} of {}", r.ranked_players));
    }
    if r.sets_played > 0 {
        parts.push(format!("{}-{} in Ranked", r.wins, r.losses));
    }
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
        None => format!("{base}/assets/icon-512.png"),
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
        stats.push_str(&stat("Record", &format!("{} W - {} L", r.wins, r.losses)));
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
      <p class=\"standing\">{standing}</p>
    </div>
  </section>
  <section class=\"card\">
    <h2>Ranked</h2>
    <dl class=\"stats\">{stats}</dl>
  </section>
  <p class=\"cta\">Brawl and P+ online with rollback netcode. <a href=\"/\">Download Brawl Online</a></p>
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
    )
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
        assert_eq!(summary(&p), "Gold 2 · Rating 1523.4 · #12 of 340 · 27-27 in Ranked");
        let html = page_html(&p, "https://brawlonline.net");
        // The name is escaped everywhere it goes.
        assert!(!html.contains("Sonder<b>"), "{html}");
        assert!(html.contains("<h1 class=\"name\">Sonder&lt;b&gt;&quot;&amp;</h1>"), "{html}");
        assert!(html.contains("\"name\":\"Sonder\\u003cb\\u003e\\\"\\u0026\""), "{html}");
        // What Discord reads.
        assert!(html.contains("<meta property=\"og:title\" content=\"Sonder&lt;b&gt;&quot;&amp; (SOND#810)\">"));
        assert!(html.contains("content=\"Gold 2 · Rating 1523.4 · #12 of 340 · 27-27 in Ranked\""));
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
    fn page_of_a_placing_and_a_new_player() {
        let p = profile(None, None, 2);
        assert_eq!(summary(&p), "Placing: 3 more ranked sets · 1-1 in Ranked");
        let html = page_html(&p, "https://brawlonline.net");
        assert!(html.contains("<dt>Rating</dt><dd>????</dd>"), "{html}");
        assert!(html.contains("3 more</span>"));
        assert!(!html.contains("Leaderboard"));
        assert!(html.contains("og:image\" content=\"https://brawlonline.net/assets/icon-512.png\""));
        let p = profile(None, None, 0);
        assert_eq!(summary(&p), "Not ranked yet");
        let html = page_html(&p, "");
        assert!(!html.contains("Rating") && !html.contains("Record"), "{html}");
        assert!(html.contains("<dt>Sets played</dt><dd>0</dd>"));
    }
}
