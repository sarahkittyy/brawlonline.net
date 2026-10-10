//! Chat groups: who chats with whom, free of I/O (`docs/chat-protocol.md`, decisions in
//! `docs/design/chat.md`).
//!
//! mm relays end-to-end encrypted chat between the members of a group and never sees the text.
//! An online connection is in at most one group:
//!
//! - **Room** groups (`room-<CODE>-<n>`, `n` the room's serial number): the room's players whose
//!   online connection has a chat identity key. The engine keeps them in step with the room
//!   ([`ChatBook::sync_room`] after every change of a room's players).
//! - **Match** groups (the `matchId` of a Direct, Unranked or Ranked match): the matched players
//!   that have an online connection with a key ([`ChatBook::start_match`]). A member leaves on
//!   `chat-leave`, a new ticket, `room-create` or `room-join`, or when its connection drops.
//!
//! Entering a group leaves the previous one; a group nobody is in is forgotten. After every change
//! each member gets `chat-group` (personalised by `you`), and a connection that left (and is still
//! there) gets `{"type": "chat-group", "group": null}`. Members are kept in joining order.
//!
//! [`ChatBook::request`] checks and carries out the `chat-*` requests. mm checks only what it can
//! without the text: the group is the sender's, boxes go to members that published a group key,
//! lengths and hex, and the rate limits. Signatures are the clients' job (and the report task's,
//! [`crate::server`]).

use std::collections::HashMap;
use std::time::{Duration, Instant};

use common::chat::{
    self, ChatError, ChatGroup, ChatMember, ChatMsg, ChatReported, ChatRequest, ReportedMessage, CHAT_ERROR,
    CHAT_GROUP, CHAT_MSG, CHAT_REPORTED, MAX_GROUP_MEMBERS,
};
use common::ratelimit::{RateLimiter, Window};
use uuid::Uuid;

use crate::engine::{ConnId, Output};
use crate::messages as msg;

#[derive(Debug, Clone)]
pub struct ChatConfig {
    /// `chat-send` per account: 5 in 5 s and 30 in a minute.
    pub send_windows: Vec<Window>,
    /// `chat-report` per account: 5 an hour.
    pub report_windows: Vec<Window>,
    /// `chat-key` and `chat-leave` per account (each `chat-key` makes mm push the group to every
    /// member, so it is limited too).
    pub key_windows: Vec<Window>,
}

impl Default for ChatConfig {
    fn default() -> Self {
        ChatConfig {
            send_windows: vec![Window::new(5, Duration::from_secs(5)), Window::new(30, Duration::from_secs(60))],
            report_windows: vec![Window::new(5, Duration::from_secs(3600))],
            key_windows: vec![Window::new(10, Duration::from_secs(10))],
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GroupKind {
    Room,
    Match,
}

impl GroupKind {
    pub fn name(self) -> &'static str {
        match self {
            GroupKind::Room => "room",
            GroupKind::Match => "match",
        }
    }
}

/// An online connection that can chat: the verified account and the identity key of its `hello`.
#[derive(Debug, Clone, PartialEq)]
pub struct Candidate {
    pub uid: Uuid,
    pub conn: ConnId,
    pub display_name: String,
    pub connect_code: String,
    pub id_key: [u8; 32],
}

/// A report for the database (`Output::ChatReport`), checked for shape; the signatures are checked
/// by the task that stores it.
#[derive(Debug, Clone, PartialEq)]
pub struct ChatReportRecord {
    pub reporter: Uuid,
    pub reported: Uuid,
    pub group: String,
    pub reason: String,
    pub messages: Vec<ReportedMessage>,
}

#[derive(Debug, Clone)]
struct Member {
    who: Candidate,
    /// The group key and its signature, once the member sent `chat-key`.
    kx: Option<([u8; 32], [u8; 64])>,
}

#[derive(Debug, Clone)]
struct Group {
    kind: GroupKind,
    members: Vec<Member>,
}

pub struct ChatBook {
    groups: HashMap<String, Group>,
    /// Account → the id of its group.
    group_of: HashMap<Uuid, String>,
    /// Online connection → its account, for the members.
    uid_of_conn: HashMap<ConnId, Uuid>,
    send_limiter: RateLimiter<Uuid>,
    report_limiter: RateLimiter<Uuid>,
    key_limiter: RateLimiter<Uuid>,
}

fn send(out: &mut Vec<Output>, conn: ConnId, m: &impl serde::Serialize) {
    let json = serde_json::to_string(m).expect("chat messages serialize");
    out.push(Output::Send { conn, json });
}

/// `chat-error`, to the sender of a refused request.
pub fn chat_error(out: &mut Vec<Output>, conn: ConnId, op: &str, error: &str) {
    send(out, conn, &ChatError { kind: CHAT_ERROR.into(), op: op.into(), error: error.into() });
}

impl ChatBook {
    pub fn new(cfg: ChatConfig) -> Self {
        ChatBook {
            groups: HashMap::new(),
            group_of: HashMap::new(),
            uid_of_conn: HashMap::new(),
            send_limiter: RateLimiter::new(&cfg.send_windows),
            report_limiter: RateLimiter::new(&cfg.report_windows),
            key_limiter: RateLimiter::new(&cfg.key_windows),
        }
    }

    pub fn group_count(&self) -> usize {
        self.groups.len()
    }

    /// The id of the group this account is in.
    pub fn group_of(&self, uid: Uuid) -> Option<&str> {
        self.group_of.get(&uid).map(String::as_str)
    }

    /// The members of a group, in joining order.
    pub fn members(&self, id: &str) -> Vec<Uuid> {
        self.groups.get(id).map(|g| g.members.iter().map(|m| m.who.uid).collect()).unwrap_or_default()
    }

    /// Makes the room group `id` hold exactly `wanted` (the room's players that can chat, in
    /// joining order): players no longer wanted leave it (and are told), new ones leave their
    /// previous group and join. An empty `wanted` ends the group.
    pub fn sync_room(&mut self, id: &str, wanted: &[Candidate], out: &mut Vec<Output>) {
        let wanted = &wanted[..wanted.len().min(MAX_GROUP_MEMBERS)];
        let mut dirty = Vec::new();
        if let Some(g) = self.groups.get_mut(id) {
            let mut i = 0;
            while i < g.members.len() {
                let m = &g.members[i];
                if wanted.iter().any(|w| w.uid == m.who.uid && w.conn == m.who.conn) {
                    i += 1;
                    continue;
                }
                let m = g.members.remove(i);
                self.group_of.remove(&m.who.uid);
                self.uid_of_conn.remove(&m.who.conn);
                send(out, m.who.conn, &ChatGroup::none());
                dirty.push(id.to_string());
            }
        }
        for w in wanted {
            let present = self
                .groups
                .get(id)
                .is_some_and(|g| g.members.iter().any(|m| m.who.uid == w.uid && m.who.conn == w.conn));
            if present {
                continue;
            }
            self.detach(w.uid, false, out, &mut dirty);
            self.groups
                .entry(id.to_string())
                .or_insert_with(|| Group { kind: GroupKind::Room, members: Vec::new() })
                .members
                .push(Member { who: w.clone(), kx: None });
            self.group_of.insert(w.uid, id.to_string());
            self.uid_of_conn.insert(w.conn, w.uid);
            dirty.push(id.to_string());
        }
        self.settle(dirty, out);
    }

    /// Starts the match group `id` with these players (the matched players that can chat). Each
    /// leaves its previous group.
    pub fn start_match(&mut self, id: &str, players: &[Candidate], out: &mut Vec<Output>) {
        if players.is_empty() {
            return;
        }
        let mut dirty = Vec::new();
        // A match id is never reused; should one be, its old members are out.
        if let Some(old) = self.groups.remove(id) {
            for m in old.members {
                self.group_of.remove(&m.who.uid);
                self.uid_of_conn.remove(&m.who.conn);
                send(out, m.who.conn, &ChatGroup::none());
            }
        }
        let mut members = Vec::new();
        for p in players.iter().take(MAX_GROUP_MEMBERS) {
            self.detach(p.uid, false, out, &mut dirty);
            members.push(Member { who: p.clone(), kx: None });
            self.group_of.insert(p.uid, id.to_string());
            self.uid_of_conn.insert(p.conn, p.uid);
        }
        tracing::info!(group = %id, members = members.len(), "match chat started");
        self.groups.insert(id.to_string(), Group { kind: GroupKind::Match, members });
        dirty.push(id.to_string());
        self.settle(dirty, out);
    }

    /// Takes an account out of its group (only if the group is of kind `only`, when given).
    /// `notify` sends it `chat-group` null. Returns whether it left a group.
    pub fn leave(&mut self, uid: Uuid, only: Option<GroupKind>, notify: bool, out: &mut Vec<Output>) -> bool {
        let Some(id) = self.group_of.get(&uid) else { return false };
        if only.is_some_and(|k| self.groups.get(id).map(|g| g.kind) != Some(k)) {
            return false;
        }
        let mut dirty = Vec::new();
        self.detach(uid, notify, out, &mut dirty);
        self.settle(dirty, out);
        true
    }

    /// An online connection went away (closed, dropped or replaced): it leaves its group, nobody
    /// tells it.
    pub fn conn_gone(&mut self, conn: ConnId, out: &mut Vec<Output>) {
        if let Some(uid) = self.uid_of_conn.get(&conn).copied() {
            self.leave(uid, None, false, out);
        }
    }

    /// Removes `uid` from its group without telling the others yet (`dirty` collects the groups
    /// to push).
    fn detach(&mut self, uid: Uuid, notify: bool, out: &mut Vec<Output>, dirty: &mut Vec<String>) {
        let Some(id) = self.group_of.remove(&uid) else { return };
        if let Some(g) = self.groups.get_mut(&id) {
            if let Some(i) = g.members.iter().position(|m| m.who.uid == uid) {
                let m = g.members.remove(i);
                self.uid_of_conn.remove(&m.who.conn);
                if notify {
                    send(out, m.who.conn, &ChatGroup::none());
                }
            }
        }
        dirty.push(id);
    }

    /// Forgets the groups in `dirty` that are empty and pushes the others to their members.
    fn settle(&mut self, mut dirty: Vec<String>, out: &mut Vec<Output>) {
        dirty.sort_unstable();
        dirty.dedup();
        for id in dirty {
            if self.groups.get(&id).is_some_and(|g| g.members.is_empty()) {
                self.groups.remove(&id);
                tracing::debug!(group = %id, "chat group ended");
            }
            self.push(&id, out);
        }
    }

    /// `chat-group` to every member of `id`.
    fn push(&self, id: &str, out: &mut Vec<Output>) {
        let Some(g) = self.groups.get(id) else { return };
        let members: Vec<ChatMember> = g
            .members
            .iter()
            .map(|m| ChatMember {
                uid: m.who.uid.to_string(),
                display_name: m.who.display_name.clone(),
                connect_code: m.who.connect_code.clone(),
                id_key: hex::encode(m.who.id_key),
                kx: m.kx.map(|(k, _)| hex::encode(k)),
                kx_sig: m.kx.map(|(_, s)| hex::encode(s)),
            })
            .collect();
        for m in &g.members {
            send(
                out,
                m.who.conn,
                &ChatGroup {
                    kind: CHAT_GROUP.into(),
                    group: Some(id.to_string()),
                    group_kind: Some(g.kind.name().into()),
                    you: Some(m.who.uid.to_string()),
                    members: Some(members.clone()),
                },
            );
        }
    }

    /// One `chat-*` request from `uid`'s online connection `conn`. A refusal is answered with
    /// `chat-error` and changes nothing.
    pub fn request(&mut self, now: Instant, conn: ConnId, uid: Uuid, req: ChatRequest, out: &mut Vec<Output>) {
        let op = req.op();
        if let Err(e) = self.handle(now, conn, uid, req, out) {
            tracing::debug!(conn, op, "chat request refused: {e}");
            chat_error(out, conn, op, e);
        }
    }

    /// The sender's group, if it is `group`.
    fn current(&self, uid: Uuid, group: &str) -> Result<&Group, &'static str> {
        match self.group_of.get(&uid) {
            Some(g) if g == group => self.groups.get(group).ok_or(msg::CHAT_NOT_IN_GROUP),
            _ => Err(msg::CHAT_NOT_IN_GROUP),
        }
    }

    fn handle(
        &mut self,
        now: Instant,
        conn: ConnId,
        uid: Uuid,
        req: ChatRequest,
        out: &mut Vec<Output>,
    ) -> Result<(), &'static str> {
        match req {
            ChatRequest::Key { group, kx, sig } => {
                self.current(uid, &group)?;
                if self.key_limiter.check(&uid, now).is_err() {
                    return Err(msg::TOO_MANY_REQUESTS);
                }
                let Some(m) = self.groups.get_mut(&group).and_then(|g| g.members.iter_mut().find(|m| m.who.uid == uid))
                else {
                    return Err(msg::CHAT_NOT_IN_GROUP);
                };
                m.kx = Some((kx, sig));
                self.push(&group, out);
            }
            ChatRequest::Leave { group } => {
                // Ignored for room groups and for a group the sender is not in.
                let is_match = self.current(uid, &group).is_ok_and(|g| g.kind == GroupKind::Match);
                if is_match {
                    if self.key_limiter.check(&uid, now).is_err() {
                        return Err(msg::TOO_MANY_REQUESTS);
                    }
                    tracing::info!(conn, group = %group, "left match chat");
                    self.leave(uid, Some(GroupKind::Match), true, out);
                }
            }
            ChatRequest::Send { group, boxes } => {
                let g = self.current(uid, &group)?;
                let me = uid.to_string();
                let mut to = Vec::with_capacity(boxes.len());
                for (recipient, sealed) in boxes {
                    if recipient == me {
                        return Err(msg::CHAT_NOT_A_MEMBER);
                    }
                    let m =
                        g.members.iter().find(|m| m.who.uid.to_string() == recipient).ok_or(msg::CHAT_NOT_A_MEMBER)?;
                    if m.kx.is_none() {
                        return Err(msg::CHAT_NO_KEY);
                    }
                    to.push((m.who.conn, sealed));
                }
                if to.is_empty() {
                    return Err(msg::CHAT_INVALID);
                }
                if self.send_limiter.check(&uid, now).is_err() {
                    return Err(msg::CHAT_TOO_FAST);
                }
                for (c, sealed) in to {
                    send(
                        out,
                        c,
                        &ChatMsg {
                            kind: CHAT_MSG.into(),
                            group: group.clone(),
                            from: me.clone(),
                            sealed: hex::encode(sealed),
                        },
                    );
                }
            }
            ChatRequest::Report { group, from, reason, messages } => {
                let reported = Uuid::parse_str(&from).map_err(|_| msg::CHAT_INVALID)?;
                if reported == uid || !chat::is_uid(&from) {
                    return Err(msg::CHAT_INVALID);
                }
                if self.report_limiter.check(&uid, now).is_err() {
                    return Err(msg::CHAT_REPORTS_TOO_OFTEN);
                }
                tracing::info!(conn, reporter = %uid, %reported, group = %group, messages = messages.len(), "chat report");
                out.push(Output::ChatReport(ChatReportRecord { reporter: uid, reported, group, reason, messages }));
                send(out, conn, &ChatReported { kind: CHAT_REPORTED.into(), from });
            }
        }
        Ok(())
    }
}

/// The database row of a report: each message checked against the reported account's identity
/// keys (`verified` counts those whose signature one of `keys` made in this group), its text
/// cleaned for reading (the signed text is kept as hex too, since it may hold anything, NUL
/// included, that jsonb cannot), the reason cleaned.
pub fn report_row(r: &ChatReportRecord, keys: &[[u8; 32]]) -> common::db::NewChatReport {
    let from = r.reported.to_string();
    let mut verified = 0;
    let messages: Vec<serde_json::Value> = r
        .messages
        .iter()
        .map(|m| {
            let ok = keys.iter().any(|k| chat::verify_message(k, &r.group, &from, m.seq, m.text.as_bytes(), &m.sig));
            verified += ok as i32;
            serde_json::json!({
                "seq": m.seq,
                "text": chat::clean_text(m.text.as_bytes(), chat::MAX_TEXT_CODE_POINTS).unwrap_or_default(),
                "raw": hex::encode(m.text.as_bytes()),
                "sig": hex::encode(m.sig),
                "verified": ok,
            })
        })
        .collect();
    common::db::NewChatReport {
        reporter: r.reporter,
        reported: r.reported,
        group_id: r.group.clone(),
        reason: chat::clean_text(r.reason.as_bytes(), chat::MAX_REASON_BYTES).unwrap_or_default(),
        total: messages.len() as i32,
        messages: serde_json::Value::Array(messages),
        verified,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn cand(n: u8) -> Candidate {
        Candidate {
            uid: Uuid::from_bytes([n; 16]),
            conn: n as ConnId,
            display_name: format!("player{n}"),
            connect_code: format!("P#{n}"),
            id_key: [n; 32],
        }
    }

    fn msgs(out: &[Output], conn: ConnId) -> Vec<Value> {
        out.iter()
            .filter_map(|o| match o {
                Output::Send { conn: c, json } if *c == conn => Some(serde_json::from_str(json).unwrap()),
                _ => None,
            })
            .collect()
    }

    fn last_group(out: &[Output], conn: ConnId) -> Value {
        msgs(out, conn).into_iter().rev().find(|m| m["type"] == "chat-group").expect("chat-group")
    }

    fn uids(group: &Value) -> Vec<String> {
        group["members"].as_array().unwrap().iter().map(|m| m["uid"].as_str().unwrap().to_string()).collect()
    }

    fn key(book: &mut ChatBook, now: Instant, n: u8, group: &str) -> Vec<Output> {
        let mut out = vec![];
        let req = ChatRequest::Key { group: group.into(), kx: [n + 100; 32], sig: [n; 64] };
        book.request(now, n as ConnId, cand(n).uid, req, &mut out);
        out
    }

    fn say(book: &mut ChatBook, now: Instant, n: u8, group: &str, to: &[u8]) -> Vec<Output> {
        let mut out = vec![];
        let boxes = to.iter().map(|t| (cand(*t).uid.to_string(), vec![*t; chat::MIN_BOX_BYTES])).collect();
        book.request(now, n as ConnId, cand(n).uid, ChatRequest::Send { group: group.into(), boxes }, &mut out);
        out
    }

    fn errors(out: &[Output], conn: ConnId) -> Vec<String> {
        msgs(out, conn)
            .into_iter()
            .filter(|m| m["type"] == "chat-error")
            .map(|m| m["error"].as_str().unwrap().to_string())
            .collect()
    }

    #[test]
    fn room_groups_follow_the_room() {
        let mut book = ChatBook::new(ChatConfig::default());
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[cand(1)], &mut out);
        let g = last_group(&out, 1);
        assert_eq!((g["group"].as_str(), g["kind"].as_str()), (Some("room-KFQB-1"), Some("room")));
        assert_eq!(g["you"], cand(1).uid.to_string());
        assert_eq!(g["members"][0]["idKey"], hex::encode([1u8; 32]));
        assert!(g["members"][0]["kx"].is_null() && g["members"][0]["kxSig"].is_null());
        assert_eq!(g["members"][0]["displayName"], "player1");
        assert_eq!(g["members"][0]["connectCode"], "P#1");

        // Two more join: every member gets the group, in joining order, personalised.
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[cand(1), cand(3), cand(2)], &mut out);
        for n in [1, 2, 3] {
            let g = last_group(&out, n);
            assert_eq!(uids(&g), [1, 3, 2].map(|i| cand(i).uid.to_string()));
            assert_eq!(g["you"], cand(n as u8).uid.to_string());
        }
        // Nothing changed: nothing is sent.
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[cand(1), cand(3), cand(2)], &mut out);
        assert!(out.is_empty());

        // 3 leaves: told null, the others get the smaller group.
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[cand(1), cand(2)], &mut out);
        assert_eq!(msgs(&out, 3), vec![serde_json::json!({"type": "chat-group", "group": null})]);
        assert_eq!(uids(&last_group(&out, 2)).len(), 2);
        assert_eq!(book.group_of(cand(3).uid), None);

        // The room closes.
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[], &mut out);
        assert_eq!(msgs(&out, 1), vec![serde_json::json!({"type": "chat-group", "group": null})]);
        assert_eq!(book.group_count(), 0);
        assert!(book.group_of.is_empty() && book.uid_of_conn.is_empty());
    }

    #[test]
    fn keys_are_published_and_replaced() {
        let mut book = ChatBook::new(ChatConfig::default());
        let now = Instant::now();
        book.sync_room("room-KFQB-1", &[cand(1), cand(2)], &mut vec![]);
        let out = key(&mut book, now, 1, "room-KFQB-1");
        for n in [1, 2] {
            let g = last_group(&out, n);
            assert_eq!(g["members"][0]["kx"], hex::encode([101u8; 32]));
            assert_eq!(g["members"][0]["kxSig"], hex::encode([1u8; 64]));
            assert!(g["members"][1]["kx"].is_null());
        }
        // A second chat-key replaces the first.
        let mut out = vec![];
        let req = ChatRequest::Key { group: "room-KFQB-1".into(), kx: [7; 32], sig: [8; 64] };
        book.request(now, 1, cand(1).uid, req, &mut out);
        assert_eq!(last_group(&out, 2)["members"][0]["kx"], hex::encode([7u8; 32]));
        // Not the sender's group: refused, nothing pushed.
        let out = key(&mut book, now, 1, "room-KFQB-2");
        assert_eq!(errors(&out, 1), [msg::CHAT_NOT_IN_GROUP]);
        assert!(msgs(&out, 2).is_empty());
        let out = key(&mut book, now, 3, "room-KFQB-1");
        assert_eq!(errors(&out, 3), [msg::CHAT_NOT_IN_GROUP]);
        // A rejoin (another connection) starts without a key.
        let mut c = cand(1);
        c.conn = 11;
        let mut out = vec![];
        book.sync_room("room-KFQB-1", &[cand(2), c], &mut out);
        let g = last_group(&out, 11);
        assert_eq!(uids(&g), [cand(2).uid.to_string(), cand(1).uid.to_string()]);
        assert!(g["members"][1]["kx"].is_null());
        // chat-key floods are limited.
        let mut refused = 0;
        for _ in 0..20 {
            refused += errors(&key(&mut book, now, 2, "room-KFQB-1"), 2).len();
        }
        assert_eq!(refused, 10);
    }

    #[test]
    fn sending_is_routed_and_checked() {
        let mut book = ChatBook::new(ChatConfig::default());
        let now = Instant::now();
        let id = "room-KFQB-1";
        book.sync_room(id, &[cand(1), cand(2), cand(3)], &mut vec![]);
        key(&mut book, now, 2, id);
        // 3 has no key yet: a box for it is refused, nothing forwarded.
        let out = say(&mut book, now, 1, id, &[2, 3]);
        assert_eq!(errors(&out, 1), [msg::CHAT_NO_KEY]);
        assert!(msgs(&out, 2).is_empty());
        let out = say(&mut book, now, 1, id, &[2]);
        let m = msgs(&out, 2);
        assert_eq!(m.len(), 1);
        assert_eq!(m[0]["type"], "chat-msg");
        assert_eq!(m[0]["group"], id);
        assert_eq!(m[0]["from"], cand(1).uid.to_string());
        assert_eq!(m[0]["box"], hex::encode(vec![2u8; chat::MIN_BOX_BYTES]));
        assert!(msgs(&out, 1).is_empty() && msgs(&out, 3).is_empty());
        // To oneself, to a stranger, to another group.
        key(&mut book, now, 1, id);
        assert_eq!(errors(&say(&mut book, now, 1, id, &[1]), 1), [msg::CHAT_NOT_A_MEMBER]);
        assert_eq!(errors(&say(&mut book, now, 1, id, &[9]), 1), [msg::CHAT_NOT_A_MEMBER]);
        assert_eq!(errors(&say(&mut book, now, 1, "room-KFQB-2", &[2]), 1), [msg::CHAT_NOT_IN_GROUP]);
        assert_eq!(errors(&say(&mut book, now, 9, id, &[2]), 9), [msg::CHAT_NOT_IN_GROUP]);
        // Back and forth.
        let out = say(&mut book, now, 2, id, &[1]);
        assert_eq!(msgs(&out, 1)[0]["from"], cand(2).uid.to_string());
    }

    #[test]
    fn sending_is_rate_limited_per_account() {
        let mut book = ChatBook::new(ChatConfig::default());
        let t0 = Instant::now();
        let id = "room-KFQB-1";
        book.sync_room(id, &[cand(1), cand(2)], &mut vec![]);
        key(&mut book, t0, 2, id);
        for i in 0..5 {
            assert!(errors(&say(&mut book, t0 + Duration::from_millis(i), 1, id, &[2]), 1).is_empty());
        }
        let out = say(&mut book, t0 + Duration::from_secs(1), 1, id, &[2]);
        assert_eq!(errors(&out, 1), [msg::CHAT_TOO_FAST]);
        assert!(msgs(&out, 2).is_empty());
        // Refused requests do not count: 5 s later 5 more go, until 30 in the minute.
        let mut sent = 5;
        for s in 1..12 {
            let t = t0 + Duration::from_secs(5 * s);
            while errors(&say(&mut book, t, 1, id, &[2]), 1).is_empty() {
                sent += 1;
            }
        }
        assert_eq!(sent, 30);
        assert!(errors(&say(&mut book, t0 + Duration::from_secs(60), 1, id, &[2]), 1).is_empty());
    }

    #[test]
    fn match_groups_and_leaving() {
        let mut book = ChatBook::new(ChatConfig::default());
        let now = Instant::now();
        book.sync_room("room-KFQB-1", &[cand(1), cand(3)], &mut vec![]);
        // 1 is matched with 2 (both online with keys): 1 leaves the room group for the match's.
        let mut out = vec![];
        let id = "mode.direct-2026-10-10T00:00:00.000Z-1";
        book.start_match(id, &[cand(1), cand(2)], &mut out);
        let g = last_group(&out, 1);
        assert_eq!((g["group"].as_str(), g["kind"].as_str()), (Some(id), Some("match")));
        assert_eq!(uids(&last_group(&out, 2)).len(), 2);
        assert_eq!(uids(&last_group(&out, 3)), [cand(3).uid.to_string()]);
        // chat-leave of the room group is ignored; of the match group leaves it.
        let mut out = vec![];
        book.request(now, 3, cand(3).uid, ChatRequest::Leave { group: "room-KFQB-1".into() }, &mut out);
        assert!(out.is_empty());
        assert_eq!(book.group_of(cand(3).uid), Some("room-KFQB-1"));
        let mut out = vec![];
        book.request(now, 2, cand(2).uid, ChatRequest::Leave { group: "room-KFQB-1".into() }, &mut out);
        assert!(out.is_empty(), "not 2's group");
        let mut out = vec![];
        book.request(now, 2, cand(2).uid, ChatRequest::Leave { group: id.into() }, &mut out);
        assert_eq!(msgs(&out, 2), vec![serde_json::json!({"type": "chat-group", "group": null})]);
        assert_eq!(uids(&last_group(&out, 1)), [cand(1).uid.to_string()]);
        // leave(Match) does nothing to a room member; the last match member's connection drops.
        assert!(!book.leave(cand(3).uid, Some(GroupKind::Match), true, &mut vec![]));
        let mut out = vec![];
        book.conn_gone(1, &mut out);
        assert!(out.is_empty(), "a gone connection is not told");
        assert_eq!(book.group_count(), 1);
        assert_eq!(book.members("room-KFQB-1"), [cand(3).uid]);
        // No players that can chat: no group.
        book.start_match("mode.unranked-x-2", &[], &mut vec![]);
        assert_eq!(book.group_count(), 1);
    }

    #[test]
    fn report_rows_count_genuine_messages() {
        let seed = [7u8; 32];
        let id = chat::identity_public(&seed);
        let (reporter, reported) = (cand(1).uid, cand(2).uid);
        let group = "room-KFQB-1";
        let from = reported.to_string();
        let text = "you\u{0} lose \u{202e}";
        let genuine = ReportedMessage {
            seq: 3,
            text: text.into(),
            sig: chat::sign_message(&seed, group, &from, 3, text.as_bytes()),
        };
        // Made up by the reporter: signed by another key, or the text changed after signing.
        let forged = ReportedMessage {
            seq: 4,
            text: "i cheat".into(),
            sig: chat::sign_message(&[8; 32], group, &from, 4, b"i cheat"),
        };
        let altered = ReportedMessage { text: "you lose!".into(), ..genuine.clone() };
        let r = ChatReportRecord {
            reporter,
            reported,
            group: group.into(),
            reason: " spam\n spam ".into(),
            messages: vec![genuine.clone(), forged, altered],
        };
        let row = report_row(&r, &[[9; 32], id]);
        assert_eq!((row.verified, row.total), (1, 3));
        assert_eq!((row.reporter, row.reported, row.group_id.as_str()), (reporter, reported, group));
        assert_eq!(row.reason, "spam spam");
        let m = row.messages.as_array().unwrap();
        assert_eq!(m[0]["verified"], true);
        assert_eq!(m[0]["text"], "you lose");
        assert_eq!(m[0]["raw"], hex::encode(text.as_bytes()));
        assert_eq!(m[0]["seq"], 3);
        assert_eq!(m[1]["verified"], false);
        assert_eq!(m[2]["verified"], false);
        // In another group the same signature proves nothing.
        let row = report_row(&ChatReportRecord { group: "room-KFQB-2".into(), ..r.clone() }, &[id]);
        assert_eq!(row.verified, 0);
        // No keys known: nothing verifies, the report is still stored.
        assert_eq!(report_row(&r, &[]).verified, 0);
    }

    #[test]
    fn reports() {
        let mut book = ChatBook::new(ChatConfig::default());
        let now = Instant::now();
        let report = |from: Uuid| ChatRequest::Report {
            group: "room-KFQB-1".into(),
            from: from.to_string(),
            reason: "spam".into(),
            messages: vec![ReportedMessage { seq: 1, text: "hi".into(), sig: [0; 64] }],
        };
        let mut out = vec![];
        book.request(now, 1, cand(1).uid, report(cand(2).uid), &mut out);
        assert_eq!(msgs(&out, 1), vec![serde_json::json!({"type": "chat-reported", "from": cand(2).uid.to_string()})]);
        let rec = out
            .iter()
            .find_map(|o| match o {
                Output::ChatReport(r) => Some(r.clone()),
                _ => None,
            })
            .unwrap();
        assert_eq!((rec.reporter, rec.reported, rec.group.as_str()), (cand(1).uid, cand(2).uid, "room-KFQB-1"));
        assert_eq!(rec.messages.len(), 1);
        // About oneself: refused.
        let mut out = vec![];
        book.request(now, 1, cand(1).uid, report(cand(1).uid), &mut out);
        assert_eq!(errors(&out, 1), [msg::CHAT_INVALID]);
        // 5 an hour.
        for _ in 0..4 {
            book.request(now, 1, cand(1).uid, report(cand(3).uid), &mut vec![]);
        }
        let mut out = vec![];
        book.request(now, 1, cand(1).uid, report(cand(3).uid), &mut out);
        assert_eq!(errors(&out, 1), [msg::CHAT_REPORTS_TOO_OFTEN]);
        assert!(!out.iter().any(|o| matches!(o, Output::ChatReport(_))));
    }
}
