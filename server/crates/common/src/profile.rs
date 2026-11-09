//! What a player can put on their profile (docs/design/profiles.md): mains, a country and social
//! media accounts. The launcher keeps the same lists (`launcher/src/common/profile.ts`); the
//! server is the one that checks them.

use crate::countries::country_name;

/// P+'s roster as a profile's mains, in the launcher's picker order, plus Random: (key, name).
/// The keys are the launcher's stock icon keys (`brawl_assets/catalog.ts`). Giga Bowser,
/// Wario-Man and Brawl's Pokemon Trainer are left out (not picked on P+'s character select).
pub const CHARACTERS: &[(&str, &str)] = &[
    ("mario", "Mario"),
    ("luigi", "Luigi"),
    ("peach", "Peach"),
    ("bowser", "Bowser"),
    ("donkey_kong", "Donkey Kong"),
    ("diddy_kong", "Diddy Kong"),
    ("yoshi", "Yoshi"),
    ("wario", "Wario"),
    ("link", "Link"),
    ("zelda", "Zelda"),
    ("sheik", "Sheik"),
    ("ganondorf", "Ganondorf"),
    ("toon_link", "Toon Link"),
    ("samus", "Samus"),
    ("zero_suit_samus", "Zero Suit Samus"),
    ("pit", "Pit"),
    ("ice_climbers", "Ice Climbers"),
    ("rob", "R.O.B."),
    ("kirby", "Kirby"),
    ("meta_knight", "Meta Knight"),
    ("king_dedede", "King Dedede"),
    ("olimar", "Olimar"),
    ("fox", "Fox"),
    ("falco", "Falco"),
    ("wolf", "Wolf"),
    ("captain_falcon", "Captain Falcon"),
    ("pikachu", "Pikachu"),
    ("squirtle", "Squirtle"),
    ("ivysaur", "Ivysaur"),
    ("charizard", "Charizard"),
    ("lucario", "Lucario"),
    ("jigglypuff", "Jigglypuff"),
    ("mewtwo", "Mewtwo"),
    ("marth", "Marth"),
    ("ike", "Ike"),
    ("roy", "Roy"),
    ("ness", "Ness"),
    ("lucas", "Lucas"),
    ("mr_game_and_watch", "Mr. Game & Watch"),
    ("snake", "Snake"),
    ("sonic", "Sonic"),
    ("knuckles", "Knuckles"),
    ("random", "Random"),
];

/// Most mains a profile may have: two rows of half-size stock icons in the launcher's profile
/// (18 a row at 16 px in its 324 px; one row of 9 at 32 px until then).
pub const MAX_MAINS: usize = 36;

/// A character's name, `None` for a key not in [`CHARACTERS`].
pub fn character_name(key: &str) -> Option<&'static str> {
    CHARACTERS.iter().find(|(k, _)| *k == key).map(|(_, n)| *n)
}

/// Why a profile change was refused (the message is shown to the player).
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ProfileError {
    #[error("Unknown character: {0}")]
    UnknownCharacter(String),
    #[error("{0} is in the mains twice")]
    DuplicateMain(String),
    #[error("At most {MAX_MAINS} mains")]
    TooManyMains,
    #[error("Unknown country: {0}")]
    UnknownCountry(String),
    #[error("{0}")]
    Social(String),
}

/// Checks a list of mains (the first is the primary); returns it as given.
pub fn check_mains(mains: &[String]) -> Result<(), ProfileError> {
    if mains.len() > MAX_MAINS {
        return Err(ProfileError::TooManyMains);
    }
    for (i, m) in mains.iter().enumerate() {
        let name = character_name(m).ok_or_else(|| ProfileError::UnknownCharacter(m.clone()))?;
        if mains[..i].contains(m) {
            return Err(ProfileError::DuplicateMain(name.to_string()));
        }
    }
    Ok(())
}

/// A country code as stored (lower case), or an error for one not in the list.
pub fn check_country(code: &str) -> Result<String, ProfileError> {
    let code = code.trim().to_ascii_lowercase();
    country_name(&code).map(|_| code.clone()).ok_or(ProfileError::UnknownCountry(code))
}

/// The social media a profile can link, in the order the profile shows them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Platform {
    Discord,
    Twitter,
    Twitch,
    Youtube,
    Tiktok,
    Instagram,
}

impl Platform {
    pub const ALL: [Platform; 6] = [
        Platform::Discord,
        Platform::Twitter,
        Platform::Twitch,
        Platform::Youtube,
        Platform::Tiktok,
        Platform::Instagram,
    ];

    /// The API's and the database column's name.
    pub fn key(self) -> &'static str {
        match self {
            Platform::Discord => "discord",
            Platform::Twitter => "twitter",
            Platform::Twitch => "twitch",
            Platform::Youtube => "youtube",
            Platform::Tiktok => "tiktok",
            Platform::Instagram => "instagram",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Platform::Discord => "Discord",
            Platform::Twitter => "X (Twitter)",
            Platform::Twitch => "Twitch",
            Platform::Youtube => "YouTube",
            Platform::Tiktok => "TikTok",
            Platform::Instagram => "Instagram",
        }
    }

    pub fn from_key(key: &str) -> Option<Platform> {
        Platform::ALL.into_iter().find(|p| p.key() == key)
    }

    /// The account's page for a stored handle (see [`Platform::parse`]); Discord has none.
    pub fn url(self, handle: &str) -> Option<String> {
        match self {
            Platform::Discord => None,
            Platform::Twitter => Some(format!("https://x.com/{handle}")),
            Platform::Twitch => Some(format!("https://twitch.tv/{handle}")),
            Platform::Youtube => Some(format!("https://youtube.com/{handle}")),
            Platform::Tiktok => Some(format!("https://tiktok.com/@{handle}")),
            Platform::Instagram => Some(format!("https://instagram.com/{handle}")),
        }
    }

    /// The handle as the profile shows it: `@name`, Discord's as it is, a YouTube channel id as
    /// its path.
    pub fn display(self, handle: &str) -> String {
        match self {
            Platform::Discord => handle.to_string(),
            Platform::Youtube => handle.to_string(),
            _ => format!("@{handle}"),
        }
    }

    /// What a player typed (a username, `@username` or a link to the account) as the handle to
    /// store; `Ok(None)` for blank (the account removed). Links are only taken for the platform's
    /// own site, so a profile never links anywhere else.
    pub fn parse(self, input: &str) -> Result<Option<String>, ProfileError> {
        let s = input.trim();
        if s.is_empty() {
            return Ok(None);
        }
        let bad = || ProfileError::Social(format!("That isn't a {} username or link.", self.name()));
        let path = match self {
            Platform::Discord => None,
            Platform::Twitter => strip_site(s, &["x.com", "twitter.com"]),
            Platform::Twitch => strip_site(s, &["twitch.tv"]),
            Platform::Youtube => strip_site(s, &["youtube.com"]),
            Platform::Tiktok => strip_site(s, &["tiktok.com"]),
            Platform::Instagram => strip_site(s, &["instagram.com"]),
        };
        // A link to another site, or a path where a name goes.
        let looks_like_link = s.contains('/') || s.contains("://");
        let from_link = path.is_some();
        let rest = match path {
            Some(p) => p,
            None if looks_like_link => return Err(bad()),
            None => s,
        };
        let rest = rest.split(['?', '#']).next().unwrap_or("").trim_end_matches('/');
        let ok = |name: &str, chars: fn(char) -> bool, len: std::ops::RangeInclusive<usize>| {
            len.contains(&name.chars().count()) && name.chars().all(chars)
        };
        let word = |c: char| c.is_ascii_alphanumeric() || c == '_';
        let dotted = |c: char| c.is_ascii_alphanumeric() || c == '_' || c == '.';
        let handle = match self {
            Platform::Discord => {
                let name = rest.to_ascii_lowercase();
                ok(&name, dotted, 2..=32).then_some(name)
            }
            Platform::Twitter => {
                let name = rest.trim_start_matches('@');
                ok(name, word, 1..=15).then(|| name.to_string())
            }
            Platform::Twitch => {
                let name = rest.trim_start_matches('@').to_ascii_lowercase();
                ok(&name, word, 3..=25).then_some(name)
            }
            Platform::Tiktok => {
                let name = rest.trim_start_matches('@');
                ok(name, dotted, 2..=24).then(|| name.to_string())
            }
            Platform::Instagram => {
                let name = rest.trim_start_matches('@');
                ok(name, dotted, 1..=30).then(|| name.to_string())
            }
            Platform::Youtube => {
                // @handle, channel/<id>, c/<name> or user/<name>; a bare name is a handle.
                let handle = |c: char| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-');
                match rest.split_once('/') {
                    Some(("channel", id)) if id.len() == 24 && id.starts_with("UC") && id.chars().all(handle) => {
                        Some(format!("channel/{id}"))
                    }
                    Some((kind @ ("c" | "user"), name)) if ok(name, handle, 1..=100) => Some(format!("{kind}/{name}")),
                    Some(_) => None,
                    // youtube.com/watch, /shorts and the like are not channels: a link needs the @.
                    None if from_link && !rest.starts_with('@') => None,
                    None => {
                        let name = rest.trim_start_matches('@');
                        ok(name, handle, 3..=30).then(|| format!("@{name}"))
                    }
                }
            }
        };
        handle.map(Some).ok_or_else(bad)
    }
}

/// The path after one of `sites` (with or without `https://`, `www.` or `m.`), or `None` when `s`
/// is not a link to one of them.
fn strip_site<'a>(s: &'a str, sites: &[&str]) -> Option<&'a str> {
    let lower = s.to_ascii_lowercase();
    let mut at = 0;
    for scheme in ["https://", "http://"] {
        if lower.starts_with(scheme) {
            at = scheme.len();
            break;
        }
    }
    for sub in ["www.", "m."] {
        if lower[at..].starts_with(sub) {
            at += sub.len();
            break;
        }
    }
    sites.iter().find_map(|site| {
        let after = lower[at..].strip_prefix(site)?;
        let path = after.strip_prefix('/')?;
        Some(&s[s.len() - path.len()..])
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(platform: Platform, s: &str) -> Option<String> {
        platform.parse(s).unwrap()
    }

    #[test]
    fn handles_and_links_become_the_handle() {
        use Platform::*;
        assert_eq!(p(Discord, " SarahKittyy "), Some("sarahkittyy".into()));
        assert!(Discord.parse("https://discord.gg/abc").is_err());
        assert_eq!(p(Twitch, "twitch.tv/SarahKitty"), Some("sarahkitty".into()));
        assert_eq!(p(Twitch, "https://www.twitch.tv/sarahkitty/"), Some("sarahkitty".into()));
        assert_eq!(p(Twitch, "@sarahkitty"), Some("sarahkitty".into()));
        assert_eq!(p(Twitter, "https://twitter.com/Sarah_K?s=20"), Some("Sarah_K".into()));
        assert_eq!(p(Twitter, "x.com/Sarah_K"), Some("Sarah_K".into()));
        assert_eq!(p(Tiktok, "https://www.tiktok.com/@sarah.k"), Some("sarah.k".into()));
        assert_eq!(p(Instagram, "instagram.com/sarah.k/"), Some("sarah.k".into()));
        assert_eq!(p(Youtube, "sarahkitty"), Some("@sarahkitty".into()));
        assert_eq!(p(Youtube, "https://m.youtube.com/@sarahkitty"), Some("@sarahkitty".into()));
        assert_eq!(
            p(Youtube, "youtube.com/channel/UC1234567890123456789012"),
            Some("channel/UC1234567890123456789012".into())
        );
        assert_eq!(p(Youtube, "youtube.com/c/Sarah"), Some("c/Sarah".into()));
        assert_eq!(p(Twitch, "   "), None);
        // Another site, a script, too long, wrong characters.
        assert!(Twitch.parse("https://evil.example/twitch.tv/x").is_err());
        assert!(Twitch.parse("javascript:alert(1)").is_err());
        assert!(Twitter.parse("abcdefghijklmnop").is_err());
        assert!(Instagram.parse("sarah k").is_err());
        assert!(Youtube.parse("youtube.com/watch?v=abc").is_err());
        assert_eq!(Twitch.url("sarahkitty").as_deref(), Some("https://twitch.tv/sarahkitty"));
        assert_eq!(Tiktok.url("sarah.k").as_deref(), Some("https://tiktok.com/@sarah.k"));
        assert_eq!(Discord.url("x"), None);
    }

    #[test]
    fn mains_and_countries() {
        let s = |v: &[&str]| v.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        assert!(check_mains(&s(&["marth", "random", "sheik"])).is_ok());
        assert!(check_mains(&[]).is_ok());
        assert_eq!(
            check_mains(&s(&["marth", "giga_bowser"])),
            Err(ProfileError::UnknownCharacter("giga_bowser".into()))
        );
        assert_eq!(check_mains(&s(&["marth", "marth"])), Err(ProfileError::DuplicateMain("Marth".into())));
        let all: Vec<String> = CHARACTERS.iter().map(|(k, _)| k.to_string()).collect();
        assert_eq!(check_mains(&all), Err(ProfileError::TooManyMains));
        assert!(check_mains(&all[..MAX_MAINS]).is_ok());
        assert_eq!(check_country(" SE ").as_deref(), Ok("se"));
        assert_eq!(check_country("gb-eng").as_deref(), Ok("gb-eng"));
        assert!(check_country("xx").is_err());
        assert!(check_country("zz").is_err());
    }
}
