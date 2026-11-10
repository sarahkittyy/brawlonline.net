//! `GET /v1/launcher`: settings the launcher reads from the server, so they change in production
//! with an env var and a restart instead of a launcher release (docs/trophy-button.md).

use axum::extract::State;
use axum::Json;
use serde::Serialize;

use crate::AppState;

#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct LauncherSettings {
    /// The header's gold trophy opens this page in the browser; null: no trophy.
    pub tournaments_url: Option<String>,
}

/// `TOURNAMENTS_URL`, trimmed: an http(s) URL, else none.
pub fn tournaments_url(raw: &str) -> Option<String> {
    let url = raw.trim();
    let lower = url.to_ascii_lowercase();
    let scheme = ["https://", "http://"].into_iter().find(|s| lower.starts_with(s))?;
    (url.len() > scheme.len() && !url.chars().any(char::is_whitespace)).then(|| url.to_string())
}

/// Public, no session: the trophy shows logged out too.
pub async fn settings(State(state): State<AppState>) -> Json<LauncherSettings> {
    Json(LauncherSettings { tournaments_url: tournaments_url(&state.cfg.tournaments_url) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_web_url_shows_the_trophy() {
        assert_eq!(tournaments_url(""), None);
        assert_eq!(tournaments_url("   "), None);
        assert_eq!(tournaments_url(" https://start.gg/catfight \n").as_deref(), Some("https://start.gg/catfight"));
        assert_eq!(tournaments_url("HTTPS://start.gg/x").as_deref(), Some("HTTPS://start.gg/x"));
        assert_eq!(tournaments_url("http://example.com").as_deref(), Some("http://example.com"));
        assert_eq!(tournaments_url("https://"), None);
        assert_eq!(tournaments_url("start.gg/catfight"), None);
        assert_eq!(tournaments_url("file:///C:/Windows/notepad.exe"), None);
        assert_eq!(tournaments_url("https://a b"), None);
    }

    #[test]
    fn the_json_names_the_field_for_the_launcher() {
        let off = serde_json::to_value(LauncherSettings { tournaments_url: None }).unwrap();
        assert_eq!(off, serde_json::json!({ "tournamentsUrl": null }));
    }
}
