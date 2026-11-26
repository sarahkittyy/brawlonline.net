//! Mentions in patch notes (docs/design/changelog.md, "Mentions"): the agent board's `@`
//! autocomplete writes a player as a token that names their Brawl Online account, their Discord
//! account, or both:
//!
//! ```text
//! @{sarahkitty bo:SARA#694 dc:123456789012345678}
//! @{sarahkitty bo:SARA#694}
//! @{milla dc:61305312321208320}
//! ```
//!
//! The words before the first `bo:` / `dc:` are the label: the name written where nothing better is
//! known (the Brawl Online name, or for a Discord-only one the Discord username). The launcher's
//! changelogs ([`render`]) show a Brawl Online one as that player's name (the launcher puts its own
//! name there: badges, main, hover card, profile), and a Discord-only one as plain `@label`. The
//! Discord bot reads the same tokens in the Markdown (`<@id>`, or plain `@label` without a Discord
//! account).

use std::collections::HashMap;
use std::ops::Range;
use std::sync::LazyLock;

use regex::Regex;
use serde::{Deserialize, Serialize};
use sqlx::PgPool;
use uuid::Uuid;

use crate::pages::esc;
use crate::profile::{name_tags, NameTag};

/// The longest label kept.
const MAX_LABEL_CHARS: usize = 64;

/// One `@{...}` in the Markdown.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Mention {
    /// Never empty: the code or `someone` when the token had none.
    pub label: String,
    /// `ABCD#123`, as [`common::codes::parse_connect_code`] writes it.
    pub code: Option<String>,
    /// A Discord user id.
    pub discord: Option<u64>,
}

static TOKEN: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"@\{([^{}\n]{1,300})\}").unwrap());

/// The token in `inner` (what is between the braces), or None when it isn't one: at least one of
/// `bo:` (a connect code) and `dc:` (17 to 20 digits), each once, after the label.
fn parse(inner: &str) -> Option<Mention> {
    let words: Vec<&str> = inner.split_whitespace().collect();
    let first = words.iter().position(|w| w.starts_with("bo:") || w.starts_with("dc:"))?;
    let (mut code, mut discord) = (None, None);
    for w in &words[first..] {
        if let Some(c) = w.strip_prefix("bo:") {
            if code.is_some() {
                return None;
            }
            code = Some(common::codes::parse_connect_code(c).ok()?.to_string());
        } else {
            // Anything else after the label: not a mention.
            let d = w.strip_prefix("dc:")?;
            let valid = discord.is_none() && (17..=20).contains(&d.len()) && d.bytes().all(|b| b.is_ascii_digit());
            discord = Some(d.parse().ok().filter(|_| valid)?);
        }
    }
    let label: String = words[..first].join(" ").trim_start_matches('@').chars().take(MAX_LABEL_CHARS).collect();
    let label = if !label.is_empty() { label } else { code.clone().unwrap_or_else(|| "someone".into()) };
    Some(Mention { label, code, discord })
}

/// Every mention in the Markdown, where it is.
pub fn find(md: &str) -> Vec<(Range<usize>, Mention)> {
    TOKEN.captures_iter(md).filter_map(|c| Some((c.get(0)?.range(), parse(&c[1])?))).collect()
}

/// The connect codes the Markdown mentions, each once.
pub fn codes(md: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for (_, m) in find(md) {
        if let Some(c) = m.code {
            if !out.contains(&c) {
                out.push(c);
            }
        }
    }
    out
}

/// A Brawl Online player a changelog mentions, as the launcher shows their name.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MentionedPlayer {
    pub uid: Uuid,
    pub display_name: String,
    pub connect_code: String,
    #[serde(flatten)]
    pub tag: NameTag,
}

/// The players with these codes now (banned ones left out), by code.
pub async fn players(pool: &PgPool, codes: &[String]) -> sqlx::Result<HashMap<String, MentionedPlayer>> {
    if codes.is_empty() {
        return Ok(HashMap::new());
    }
    let rows: Vec<(Uuid, String, String)> = sqlx::query_as(
        "SELECT uid, display_name, connect_code FROM users
          WHERE connect_code = ANY($1) AND (banned_until IS NULL OR banned_until <= now())",
    )
    .bind(codes)
    .fetch_all(pool)
    .await?;
    let uids: Vec<Uuid> = rows.iter().map(|r| r.0).collect();
    let mut tags = name_tags(pool, &uids).await?;
    Ok(rows
        .into_iter()
        .map(|(uid, display_name, code)| {
            let tag = tags.remove(&uid).unwrap_or_default();
            (code.clone(), MentionedPlayer { uid, display_name, connect_code: code, tag })
        })
        .collect())
}

/// Stand-ins for the tokens while the Markdown renders: private-use characters, which the renderer
/// writes as they are, nobody types and a heading's id leaves out: `OPEN`, the token's number as
/// the character `INDEX + n`, `CLOSE`.
const OPEN: char = '\u{E000}';
const CLOSE: char = '\u{E001}';
const INDEX: u32 = 0xE100;
/// The most mentions one changelog renders (any after them stay as written).
const MAX_MENTIONS: usize = 0x700;

/// A mention as the changelog's HTML has it. `class="mention"` and `data-uid` are what the launcher
/// reads (it puts the player's own name there); `data-code` / `data-discord` are for the board's
/// preview. The text is what anything else shows (an older launcher: `@name`).
fn span(m: &Mention, players: &HashMap<String, MentionedPlayer>) -> String {
    let player = m.code.as_ref().and_then(|c| players.get(c));
    let mut attrs = String::new();
    if let Some(p) = player {
        attrs.push_str(&format!(" data-uid=\"{}\"", p.uid));
    }
    if let Some(c) = &m.code {
        attrs.push_str(&format!(" data-code=\"{}\"", esc(c)));
    }
    if let Some(d) = m.discord {
        attrs.push_str(&format!(" data-discord=\"{d}\""));
    }
    let name = player.map_or(m.label.as_str(), |p| p.display_name.as_str());
    format!("<span class=\"mention\"{attrs}>@{}</span>", esc(name))
}

/// `blog::render`'s HTML of a changelog, its mentions as [`span`]s (`players`: those found, by
/// code; an unknown code is its label). In code (`` `x` ``, a code block) a token stays as written,
/// and in an attribute (a picture's alt text) it is `@label`.
pub fn render(md: &str, players: &HashMap<String, MentionedPlayer>) -> String {
    let mut found = find(md);
    found.truncate(MAX_MENTIONS);
    if found.is_empty() {
        return crate::blog::render(md).html;
    }
    let mut marked = String::with_capacity(md.len());
    let mut at = 0;
    for (i, (range, _)) in found.iter().enumerate() {
        marked.push_str(&md[at..range.start]);
        marked.push(OPEN);
        marked.push(char::from_u32(INDEX + i as u32).unwrap_or(CLOSE));
        marked.push(CLOSE);
        at = range.end;
    }
    marked.push_str(&md[at..]);
    let html = crate::blog::render(&marked).html;

    let mut out = String::with_capacity(html.len() + found.len() * 80);
    let (mut in_tag, mut code_depth) = (false, 0usize);
    let mut rest = html.as_str();
    while let Some(c) = rest.chars().next() {
        if c == OPEN {
            let mut next = rest[OPEN.len_utf8()..].chars();
            let token = match (next.next(), next.next()) {
                (Some(n), Some(CLOSE)) => (n as u32).checked_sub(INDEX).and_then(|i| found.get(i as usize)),
                _ => None,
            };
            if let Some((range, m)) = token {
                if in_tag {
                    out.push_str(&esc(&format!("@{}", m.label)));
                } else if code_depth > 0 {
                    out.push_str(&esc(&md[range.clone()]));
                } else {
                    out.push_str(&span(m, players));
                }
                // Three characters of three bytes each.
                rest = &rest[OPEN.len_utf8() * 3..];
                continue;
            }
        }
        if c == '<' && !in_tag {
            in_tag = true;
            if rest.starts_with("<code") || rest.starts_with("<pre") {
                code_depth += 1;
            } else if rest.starts_with("</code") || rest.starts_with("</pre") {
                code_depth = code_depth.saturating_sub(1);
            }
        } else if c == '>' && in_tag {
            in_tag = false;
        }
        out.push(c);
        rest = &rest[c.len_utf8()..];
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sara() -> HashMap<String, MentionedPlayer> {
        let uid = Uuid::parse_str("24a184c8-608e-46b9-9e7b-d26d805c545c").unwrap();
        let p = MentionedPlayer {
            uid,
            display_name: "sarahkitty".into(),
            connect_code: "SARA#694".into(),
            tag: NameTag::default(),
        };
        HashMap::from([("SARA#694".to_string(), p)])
    }

    #[test]
    fn tokens() {
        let md = "thanks @{sarah kitty bo:sara#694 dc:123456789012345678} and @{milla dc:61305312321208320}, \
                  @{x bo:SARA#694 bo:SARA#694} @{nobody} @{dc:1234} @{bo:ABCD#12}";
        let f: Vec<Mention> = find(md).into_iter().map(|(_, m)| m).collect();
        assert_eq!(
            f,
            vec![
                Mention {
                    label: "sarah kitty".into(),
                    code: Some("SARA#694".into()),
                    discord: Some(123456789012345678)
                },
                Mention { label: "milla".into(), code: None, discord: Some(61305312321208320) },
                Mention { label: "ABCD#12".into(), code: Some("ABCD#12".into()), discord: None },
            ]
        );
        assert_eq!(codes(md), vec!["SARA#694", "ABCD#12"]);
    }

    #[test]
    fn renders_spans() {
        let md = "# Thanks @{sarahkitty bo:SARA#694}\n\nby @{old_name bo:SARA#694 dc:123456789012345678}, \
                  @{milla dc:61305312321208320} and @{gone bo:GONE#1}.\n\n`@{x bo:SARA#694}`\n\n\
                  ![pic of @{sarahkitty bo:SARA#694}](/blog/media/0123456789abcdef0123.webp)\n";
        let html = render(md, &sara());
        assert!(html.contains("<h2 id=\"thanks\">Thanks <span class=\"mention\" data-uid=\"24a184c8-608e-46b9-9e7b-d26d805c545c\" data-code=\"SARA#694\">@sarahkitty</span></h2>"), "{html}");
        // The player's name now, whatever the label says.
        assert!(
            html.contains("data-code=\"SARA#694\" data-discord=\"123456789012345678\">@sarahkitty</span>"),
            "{html}"
        );
        assert!(html.contains("<span class=\"mention\" data-discord=\"61305312321208320\">@milla</span>"), "{html}");
        assert!(html.contains("<span class=\"mention\" data-code=\"GONE#1\">@gone</span>"), "{html}");
        assert!(html.contains("<code>@{x bo:SARA#694}</code>"), "{html}");
        assert!(html.contains("alt=\"pic of @sarahkitty\""), "{html}");
        assert!(!html.contains(OPEN) && !html.contains(CLOSE), "{html}");
        // Nothing to do: the blog's own HTML.
        assert_eq!(render("a @b", &HashMap::new()), crate::blog::render("a @b").html);
    }

    #[test]
    fn labels_are_escaped() {
        let html = render("@{<b>&\"x bo:SARA#1}", &HashMap::new());
        assert!(html.contains(">@&lt;b&gt;&amp;&quot;x</span>"), "{html}");
    }
}
