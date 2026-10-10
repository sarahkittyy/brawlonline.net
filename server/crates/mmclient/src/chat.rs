//! A chat member (`docs/chat-protocol.md`): what Dolphin's `Online::Chat` does, for tests, people
//! at a terminal and interop checks against Dolphin.
//!
//! [`ChatSession`] holds an identity key and the state of the current group. It does no I/O: feed
//! it every message mm sends on the online connection ([`ChatSession::on_message`]) and send the
//! requests it returns; [`ChatSession::compose`] seals a message for every member whose group key
//! verified. Everything it reports is one JSON object per event (`mmclient-chat-*`), so the
//! `mmclient` binary prints them as JSON lines.
//!
//! Incoming messages go through the receiver's checks of `docs/design/chat.md` §3 in order (the
//! group, the sender's verified group key, hex and length, decryption, version and sequence,
//! signature, the text cleaner); a message that fails one is dropped with an
//! `mmclient-chat-drop` event naming the reason.

use std::path::Path;

use common::chat::{self, ChatGroup, ChatRequest, ReportedMessage, MAX_REPORT_MESSAGES, MAX_TEXT_CODE_POINTS};
use serde_json::{json, Value};

/// Messages kept per group (for reports), like Dolphin's history.
const HISTORY: usize = 100;
/// mm refuses packets above 8 KiB; a report is trimmed (oldest messages first) to stay below.
const MAX_REPORT_JSON: usize = 8000;

/// An install's chat identity: an Ed25519 seed.
#[derive(Clone)]
pub struct Identity {
    seed: [u8; 32],
}

impl std::fmt::Debug for Identity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Identity({})", self.public_hex())
    }
}

impl Identity {
    pub fn generate() -> Self {
        Identity { seed: chat::random_secret() }
    }

    pub fn from_seed(seed: [u8; 32]) -> Self {
        Identity { seed }
    }

    /// The seed in a file of 64 hex digits (as Dolphin's `chat-identity.key`), made if missing.
    pub fn load_or_create(path: &Path) -> anyhow::Result<Self> {
        if let Ok(text) = std::fs::read_to_string(path) {
            let seed = chat::decode_hex_exact::<32>(text.trim())
                .ok_or_else(|| anyhow::anyhow!("{} is not 64 hex digits", path.display()))?;
            return Ok(Identity { seed });
        }
        let id = Self::generate();
        std::fs::write(path, hex::encode(id.seed))?;
        Ok(id)
    }

    pub fn public(&self) -> [u8; 32] {
        chat::identity_public(&self.seed)
    }

    /// `hello.chatKey`.
    pub fn public_hex(&self) -> String {
        hex::encode(self.public())
    }
}

#[derive(Debug, Clone)]
struct Peer {
    uid: String,
    name: String,
    id_key: [u8; 32],
    /// Their group key, once its signature verified.
    kx: Option<[u8; 32]>,
    /// The key this pair shares.
    key: Option<[u8; 32]>,
    last_seq: u64,
}

#[derive(Debug, Clone)]
struct Group {
    id: String,
    kind: String,
    me: String,
    kx_secret: [u8; 32],
    kx_public: [u8; 32],
    /// `chat-key` was sent for this group.
    published: bool,
    peers: Vec<Peer>,
    seq: u64,
    /// What arrived: (from, message) for reports.
    received: Vec<(String, ReportedMessage)>,
}

#[derive(Debug)]
pub struct ChatSession {
    id: Identity,
    group: Option<Group>,
}

fn drop_event(group: &str, from: &str, reason: &str) -> Value {
    json!({"type": "mmclient-chat-drop", "group": group, "from": from, "reason": reason})
}

impl ChatSession {
    pub fn new(id: Identity) -> Self {
        ChatSession { id, group: None }
    }

    pub fn identity(&self) -> &Identity {
        &self.id
    }

    /// The current group's id.
    pub fn group(&self) -> Option<&str> {
        self.group.as_ref().map(|g| g.id.as_str())
    }

    /// The members that can read what this session sends (their group key verified).
    pub fn readers(&self) -> Vec<String> {
        self.group
            .as_ref()
            .map(|g| g.peers.iter().filter(|p| p.key.is_some()).map(|p| p.uid.clone()).collect())
            .unwrap_or_default()
    }

    /// Handles one message from mm (anything that is not `chat-group` or `chat-msg` is ignored).
    /// Returns the events to report and the requests to send.
    pub fn on_message(&mut self, m: &Value) -> (Vec<Value>, Vec<Value>) {
        match m["type"].as_str() {
            Some(chat::CHAT_GROUP) => self.on_group(m),
            Some(chat::CHAT_MSG) => (vec![self.on_msg(m)], vec![]),
            _ => (vec![], vec![]),
        }
    }

    fn on_group(&mut self, m: &Value) -> (Vec<Value>, Vec<Value>) {
        let parsed = match serde_json::from_value::<ChatGroup>(m.clone()) {
            Ok(g) => g,
            Err(e) => return (vec![drop_event("", "", &format!("bad chat-group: {e}"))], vec![]),
        };
        let Some(id) = parsed.group.filter(|g| chat::is_group_id(g)) else {
            self.group = None;
            return (vec![json!({"type": "mmclient-chat-group", "group": null})], vec![]);
        };
        let (Some(me), Some(members)) = (parsed.you.filter(|u| chat::is_uid(u)), parsed.members) else {
            return (vec![drop_event(&id, "", "chat-group without you or members")], vec![]);
        };
        if members.len() > chat::MAX_GROUP_MEMBERS {
            return (vec![drop_event(&id, "", "too many members")], vec![]);
        }
        if self.group.as_ref().is_none_or(|g| g.id != id || g.me != me) {
            let kx_secret = chat::random_secret();
            self.group = Some(Group {
                id: id.clone(),
                kind: parsed.group_kind.unwrap_or_default(),
                me: me.clone(),
                kx_secret,
                kx_public: chat::kx_public(&kx_secret),
                published: false,
                peers: Vec::new(),
                seq: 0,
                received: Vec::new(),
            });
        }
        let Some(g) = self.group.as_mut() else { return (vec![], vec![]) };
        let mut peers = Vec::new();
        let mut mine_listed = None;
        for mem in &members {
            if !chat::is_uid(&mem.uid) {
                continue;
            }
            let Some(id_key) = chat::parse_identity_key(&mem.id_key) else { continue };
            let kx = mem.kx.as_deref().and_then(chat::decode_hex_exact::<32>);
            let sig = mem.kx_sig.as_deref().and_then(chat::decode_hex_exact::<64>);
            if mem.uid == me {
                mine_listed = Some(kx);
                continue;
            }
            let verified = match (kx, sig) {
                (Some(kx), Some(sig)) if chat::verify_kx(&id_key, &id, &kx, &sig) => Some(kx),
                _ => None,
            };
            let old = g.peers.iter().find(|p| p.uid == mem.uid && p.id_key == id_key);
            let key = match (verified, old) {
                (Some(kx), Some(o)) if o.kx == Some(kx) => o.key,
                (Some(kx), _) => chat::pair_key(&id, &g.kx_secret, &kx),
                (None, _) => None,
            };
            peers.push(Peer {
                uid: mem.uid.clone(),
                name: chat::clean_text(mem.display_name.as_bytes(), chat::MAX_NAME_CODE_POINTS).unwrap_or_default(),
                id_key,
                kx: verified,
                key,
                last_seq: old.map(|o| o.last_seq).unwrap_or(0),
            });
        }
        g.peers = peers;
        let mut requests = vec![];
        if mine_listed.is_some_and(|k| k != Some(g.kx_public)) && !g.published {
            let sig = chat::sign_kx(&self.id.seed, &g.id, &g.kx_public);
            requests.push(ChatRequest::Key { group: g.id.clone(), kx: g.kx_public, sig }.to_json());
            g.published = true;
        }
        let event = json!({
            "type": "mmclient-chat-group",
            "group": g.id,
            "kind": g.kind,
            "you": g.me,
            "members": g.peers.iter().map(|p| json!({"uid": p.uid, "displayName": p.name, "canRead": p.key.is_some()})).collect::<Vec<_>>(),
        });
        (vec![event], requests)
    }

    fn on_msg(&mut self, m: &Value) -> Value {
        let (group, from) = (m["group"].as_str().unwrap_or(""), m["from"].as_str().unwrap_or(""));
        let Some(g) = self.group.as_mut().filter(|g| g.id == group) else {
            return drop_event(group, from, "not the current group");
        };
        let Some(peer) = g.peers.iter_mut().find(|p| p.uid == from) else {
            return drop_event(group, from, "sender is not a member");
        };
        let Some(key) = peer.key else { return drop_event(group, from, "sender has no verified group key") };
        let Some(sealed) = m["box"].as_str().and_then(|b| chat::decode_hex(b, chat::MAX_BOX_BYTES)) else {
            return drop_event(group, from, "box is not hex of at most 512 bytes");
        };
        let Some(plain) = chat::open(&key, &chat::box_ad(group, from, &g.me), &sealed) else {
            return drop_event(group, from, "box does not open");
        };
        let Some((seq, sig, text)) = chat::parse_plaintext(&plain) else {
            return drop_event(group, from, "bad plaintext");
        };
        if seq <= peer.last_seq {
            return drop_event(group, from, "replayed or old sequence number");
        }
        if !chat::verify_message(&peer.id_key, group, from, seq, text, &sig) {
            return drop_event(group, from, "bad signature");
        }
        let Some(clean) = chat::clean_text(text, MAX_TEXT_CODE_POINTS) else {
            return drop_event(group, from, "nothing left after cleaning");
        };
        peer.last_seq = seq;
        let name = peer.name.clone();
        // clean_text accepted it, so it is UTF-8.
        let raw = String::from_utf8_lossy(text).into_owned();
        g.received.push((from.to_string(), ReportedMessage { seq, text: raw, sig }));
        if g.received.len() > HISTORY {
            g.received.remove(0);
        }
        json!({"type": "mmclient-chat-msg", "group": group, "from": from, "displayName": name, "seq": seq, "text": clean})
    }

    /// Seals `text` (cleaned first) for every member that can read it: the `chat-send` request
    /// and an `mmclient-chat-sent` event. `Err` says why nothing can be sent.
    pub fn compose(&mut self, text: &str) -> Result<(Value, Value), String> {
        let clean = chat::clean_text(text.as_bytes(), MAX_TEXT_CODE_POINTS).ok_or("nothing left after cleaning")?;
        let g = self.group.as_mut().ok_or("not in a chat group")?;
        let readers: Vec<&Peer> = g.peers.iter().filter(|p| p.key.is_some()).collect();
        if readers.is_empty() {
            return Err("nobody in the group can read chat yet".into());
        }
        g.seq += 1;
        let sig = chat::sign_message(&self.id.seed, &g.id, &g.me, g.seq, clean.as_bytes());
        let plain = chat::encode_plaintext(g.seq, &sig, clean.as_bytes());
        let mut boxes = Vec::new();
        for p in &readers {
            let nonce: [u8; 24] = rand::random();
            let key = p.key.expect("filtered");
            boxes.push((p.uid.clone(), chat::seal(&key, &nonce, &chat::box_ad(&g.id, &g.me, &p.uid), &plain)));
        }
        let to: Vec<String> = boxes.iter().map(|(u, _)| u.clone()).collect();
        let event = json!({"type": "mmclient-chat-sent", "group": g.id, "seq": g.seq, "text": clean, "to": to});
        Ok((ChatRequest::Send { group: g.id.clone(), boxes }.to_json(), event))
    }

    /// The messages received from `from` in the current group, oldest first.
    pub fn received_from(&self, from: &str) -> Vec<ReportedMessage> {
        self.group
            .as_ref()
            .map(|g| g.received.iter().filter(|(f, _)| f == from).map(|(_, m)| m.clone()).collect())
            .unwrap_or_default()
    }

    /// A `chat-report` about `from` with the newest messages received from it (at most 20, and
    /// as many as fit one packet).
    pub fn report(&self, from: &str, reason: &str) -> Option<Value> {
        let g = self.group.as_ref()?;
        let mut messages = self.received_from(from);
        let skip = messages.len().saturating_sub(MAX_REPORT_MESSAGES);
        messages.drain(..skip);
        let reason: String = reason
            .chars()
            .scan(0, |n, c| {
                *n += c.len_utf8();
                (*n <= chat::MAX_REASON_BYTES).then_some(c)
            })
            .collect();
        loop {
            let req = ChatRequest::Report {
                group: g.id.clone(),
                from: from.into(),
                reason: reason.clone(),
                messages: messages.clone(),
            };
            let v = req.to_json();
            if v.to_string().len() <= MAX_REPORT_JSON || messages.is_empty() {
                return Some(v);
            }
            messages.remove(0);
        }
    }

    /// `chat-leave` for the current group (mm ignores it for a room's group).
    pub fn leave(&self) -> Option<Value> {
        self.group.as_ref().map(|g| ChatRequest::Leave { group: g.id.clone() }.to_json())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "11111111-1111-4111-8111-111111111111";
    const B: &str = "22222222-2222-4222-8222-222222222222";

    /// A member as mm lists it: uid, its session (for the identity key), its published group key.
    type Listed<'a> = (&'a str, &'a ChatSession, Option<(String, String)>);

    /// What mm would push: the group with the given members' keys.
    fn group_msg(you: &str, members: &[Listed]) -> Value {
        json!({
            "type": "chat-group", "group": "room-KFQB-1", "kind": "room", "you": you,
            "members": members.iter().map(|(uid, s, kx)| json!({
                "uid": uid, "displayName": format!("n{}", &uid[..1]), "connectCode": "X#1",
                "idKey": s.identity().public_hex(),
                "kx": kx.as_ref().map(|k| k.0.clone()), "kxSig": kx.as_ref().map(|k| k.1.clone()),
            })).collect::<Vec<_>>(),
        })
    }

    fn published(reqs: &[Value]) -> (String, String) {
        let r = reqs.iter().find(|r| r["type"] == "chat-key").expect("chat-key");
        (r["kx"].as_str().unwrap().into(), r["sig"].as_str().unwrap().into())
    }

    /// Two sessions in one group, keys exchanged as mm would relay them.
    fn pair() -> (ChatSession, ChatSession) {
        let (mut a, mut b) = (ChatSession::new(Identity::generate()), ChatSession::new(Identity::generate()));
        let (_, ra) = a.on_message(&group_msg(A, &[(A, &a, None), (B, &b, None)]));
        let (_, rb) = b.on_message(&group_msg(B, &[(A, &a, None), (B, &b, None)]));
        let (ka, kb) = (published(&ra), published(&rb));
        let full_a = group_msg(A, &[(A, &a, Some(ka.clone())), (B, &b, Some(kb.clone()))]);
        let full_b = group_msg(B, &[(A, &a, Some(ka)), (B, &b, Some(kb))]);
        let (_, again) = a.on_message(&full_a);
        assert!(again.is_empty(), "published once");
        b.on_message(&full_b);
        (a, b)
    }

    fn relay(req: &Value, from: &str, to: &str) -> Value {
        json!({"type": "chat-msg", "group": req["group"], "from": from, "box": req["boxes"][to]})
    }

    #[test]
    fn messages_round_trip_and_replays_are_dropped() {
        let (mut a, mut b) = pair();
        assert_eq!(a.readers(), [B]);
        let (req, sent) = a.compose("  hi \u{202e}there ").unwrap();
        assert_eq!(sent["text"], "hi there");
        let msg = relay(&req, A, B);
        let (ev, _) = b.on_message(&msg);
        assert_eq!(ev[0]["type"], "mmclient-chat-msg");
        assert_eq!((ev[0]["text"].as_str(), ev[0]["seq"].as_u64()), (Some("hi there"), Some(1)));
        let (ev, _) = b.on_message(&msg);
        assert_eq!(ev[0]["reason"], "replayed or old sequence number");
        // Addressed to someone else (the AD names the recipient): does not open.
        let mut wrong = msg.clone();
        wrong["from"] = json!(B);
        let (ev, _) = a.on_message(&wrong);
        assert_eq!(ev[0]["type"], "mmclient-chat-drop");
        // A report carries the signed text; mm's check accepts it.
        let r = b.report(A, "rude").unwrap();
        assert_eq!(r["messages"][0]["text"], "hi there");
        let sig = chat::decode_hex_exact::<64>(r["messages"][0]["sig"].as_str().unwrap()).unwrap();
        assert!(chat::verify_message(&a.identity().public(), "room-KFQB-1", A, 1, b"hi there", &sig));
    }

    #[test]
    fn nothing_is_sent_before_a_key_verifies() {
        let (mut a, b) = (ChatSession::new(Identity::generate()), ChatSession::new(Identity::generate()));
        a.on_message(&group_msg(A, &[(A, &a, None), (B, &b, None)]));
        assert!(a.compose("hi").is_err());
        // A group key signed by someone else is not taken.
        let forged = (hex::encode([9u8; 32]), hex::encode([1u8; 64]));
        a.on_message(&group_msg(A, &[(A, &a, None), (B, &b, Some(forged))]));
        assert!(a.readers().is_empty());
        assert!(a.compose("hi").is_err());
        let (ev, _) = a.on_message(&json!({"type": "chat-group", "group": null}));
        assert_eq!(ev[0], json!({"type": "mmclient-chat-group", "group": null}));
        assert_eq!(a.group(), None);
    }
}
