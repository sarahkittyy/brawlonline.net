//! Code shared by the `accounts` web service, the `mm` matchmaking server, the
//! `admin` CLI and the `mmclient` test client.
//!
//! Parts of [`codes`] and [`proto`] are derived from openmelee
//! (<https://github.com/panchaea/openmelee>, commit `bbbbff5`, GPL-2.0), an
//! open-source reimplementation of the Slippi matchmaking server. See the
//! module docs for what was taken.

pub mod chat;
pub mod codes;
pub mod db;
pub mod net;
pub mod playkey;
pub mod proto;
pub mod ranked;
pub mod ratelimit;
pub mod rooms;

/// The product name shown to users: email subjects and bodies, the web pages, the default sender
/// name. Hostnames are configuration (`PUBLIC_BASE_URL`, `MAIL_FROM`, the Caddyfile).
pub const PRODUCT_NAME: &str = "Brawl Online";

/// The 16 default quick-chat messages, in Slippi's order
/// (`slippi-rust-extensions/user/src/chat.rs`). Sent in `get-ticket-resp` and from `/user/{uid}`
/// because the Slippi client falls back to defaults unless it gets exactly 16.
pub const DEFAULT_CHAT_MESSAGES: [&str; 16] = [
    "ggs",
    "one more",
    "brb",
    "good luck",
    "well played",
    "that was fun",
    "thanks",
    "too good",
    "sorry",
    "my b",
    "lol",
    "wow",
    "gotta go",
    "one sec",
    "let's play again later",
    "bad connection",
];
