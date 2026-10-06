//! The two pages email links open. They are plain HTML with no styling or
//! images: the real website (sign-up, profile, leaderboard) is Phase 5.

use axum::extract::{Form, Query, State};
use axum::http::StatusCode;
use axum::response::Html;
use serde::Deserialize;

use crate::store::{self, TokenPurpose};
use crate::AppState;

#[derive(Debug, Deserialize)]
pub struct TokenQuery {
    #[serde(default)]
    pub token: String,
}

fn esc(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

fn page(title: &str, body: &str) -> Html<String> {
    Html(format!(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\
         <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
         <meta name=\"referrer\" content=\"no-referrer\"><title>{}</title></head>\
         <body><h1>{}</h1>{}</body></html>",
        esc(title),
        esc(title),
        body
    ))
}

pub async fn verify_email_page(
    State(state): State<AppState>,
    Query(q): Query<TokenQuery>,
) -> (StatusCode, Html<String>) {
    match store::verify_email(&state.pool, q.token.trim()).await {
        Ok(Some(_)) => {
            (StatusCode::OK, page("Email verified", "<p>Your email is verified. You can go back to the launcher.</p>"))
        }
        Ok(None) => (
            StatusCode::BAD_REQUEST,
            page(
                "Link expired",
                "<p>This link is invalid, already used or expired. Ask the launcher for a new one.</p>",
            ),
        ),
        Err(e) => {
            tracing::error!("verify page: {e}");
            (StatusCode::INTERNAL_SERVER_ERROR, page("Error", "<p>Something went wrong. Try again later.</p>"))
        }
    }
}

fn reset_form(token: &str, error: Option<&str>) -> String {
    format!(
        "{}<form method=\"post\" action=\"reset-password\">\
         <input type=\"hidden\" name=\"token\" value=\"{}\">\
         <p><label>New password<br><input type=\"password\" name=\"password\" minlength=\"8\" maxlength=\"256\" required autocomplete=\"new-password\"></label></p>\
         <p><label>Repeat it<br><input type=\"password\" name=\"password2\" minlength=\"8\" maxlength=\"256\" required autocomplete=\"new-password\"></label></p>\
         <p><button type=\"submit\">Set password</button></p></form>",
        error.map(|e| format!("<p><strong>{}</strong></p>", esc(e))).unwrap_or_default(),
        esc(token)
    )
}

pub async fn reset_password_page(
    State(state): State<AppState>,
    Query(q): Query<TokenQuery>,
) -> (StatusCode, Html<String>) {
    match store::peek_email_token(&state.pool, q.token.trim(), TokenPurpose::ResetPassword).await {
        Ok(Some(_)) => (StatusCode::OK, page("Choose a new password", &reset_form(q.token.trim(), None))),
        Ok(None) => (
            StatusCode::BAD_REQUEST,
            page("Link expired", "<p>This link is invalid, already used or expired. Request a new one.</p>"),
        ),
        Err(e) => {
            tracing::error!("reset page: {e}");
            (StatusCode::INTERNAL_SERVER_ERROR, page("Error", "<p>Something went wrong. Try again later.</p>"))
        }
    }
}

#[derive(Debug, Deserialize)]
pub struct ResetForm {
    pub token: String,
    pub password: String,
    pub password2: String,
}

pub async fn reset_password_submit(
    State(state): State<AppState>,
    Form(f): Form<ResetForm>,
) -> (StatusCode, Html<String>) {
    let token = f.token.trim().to_string();
    if f.password != f.password2 {
        return (
            StatusCode::BAD_REQUEST,
            page("Choose a new password", &reset_form(&token, Some("The passwords do not match"))),
        );
    }
    match crate::api::reset_password_confirm(&state, &token, f.password).await {
        Ok(_) => (
            StatusCode::OK,
            page(
                "Password changed",
                "<p>Your password is changed. Log in again in the launcher. Your play key was renewed, so the launcher will update your game login.</p>",
            ),
        ),
        Err(e) if e.code == "invalid_password" => (
            StatusCode::BAD_REQUEST,
            page("Choose a new password", &reset_form(&token, Some(&e.message))),
        ),
        Err(e) => (e.status, page("Could not change password", &format!("<p>{}</p>", esc(&e.message)))),
    }
}
