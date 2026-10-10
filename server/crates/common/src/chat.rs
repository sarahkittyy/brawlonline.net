//! Chat: the wire types of the chat protocol, the checks every field goes through, the crypto and
//! the text cleaner (`docs/chat-protocol.md`; decisions in `docs/design/chat.md`).
//!
//! mm relays end-to-end encrypted messages between the members of a chat group (a room's players,
//! or the players of a Direct, Unranked or Ranked match) over their online connections. mm sees
//! group keys, boxes and signatures, never text. Each install has an Ed25519 identity key
//! (`hello.chatKey`); each member makes an X25519 key pair per group and signs its public half
//! with the identity key (`chat-key`); each pair of members derives its own key, and a message is
//! sealed once per recipient (XChaCha20-Poly1305) with the sender's signature inside, so a
//! reported message proves who sent it in which group.
//!
//! The crypto is byte for byte what `docs/chat-protocol.md` §4 defines, so Dolphin (Monocypher)
//! and Rust interoperate: [`test_vectors`](tests) pins every value. Everything a peer sends is
//! hostile: the parsers here check every length, every hex digit and every uid before anything
//! else looks at it, and never panic.

use blake2::digest::consts::U32;
use blake2::{Blake2b, Digest};
use chacha20poly1305::aead::{AeadInPlace, KeyInit};
use chacha20poly1305::{Key, Tag, XChaCha20Poly1305, XNonce};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use x25519_dalek::{PublicKey, StaticSecret};

pub const CHAT_KEY: &str = "chat-key";
pub const CHAT_SEND: &str = "chat-send";
pub const CHAT_LEAVE: &str = "chat-leave";
pub const CHAT_REPORT: &str = "chat-report";
pub const CHAT_GROUP: &str = "chat-group";
pub const CHAT_MSG: &str = "chat-msg";
pub const CHAT_REPORTED: &str = "chat-reported";
pub const CHAT_ERROR: &str = "chat-error";

/// Group ids: at most this many characters of [`is_group_id_char`].
pub const MAX_GROUP_ID_LEN: usize = 96;
/// Members of a group at most (a room's four slots).
pub const MAX_GROUP_MEMBERS: usize = 4;
/// A message's text: at most this many bytes ...
pub const MAX_TEXT_BYTES: usize = 300;
/// ... and this many code points (after [`clean_text`]).
pub const MAX_TEXT_CODE_POINTS: usize = 100;
/// Names shown in the chat go through [`clean_text`] with this limit.
pub const MAX_NAME_CODE_POINTS: usize = 32;
/// The largest box mm forwards.
pub const MAX_BOX_BYTES: usize = 512;
/// What a plaintext holds besides the text: version, `u64le(seq)`, signature.
pub const PLAINTEXT_OVERHEAD: usize = 1 + 8 + 64;
/// The smallest box that can hold a message: nonce, MAC, a plaintext with one byte of text.
pub const MIN_BOX_BYTES: usize = 24 + 16 + PLAINTEXT_OVERHEAD + 1;
/// A report carries at most this many messages ...
pub const MAX_REPORT_MESSAGES: usize = 20;
/// ... and a reason of at most this many bytes.
pub const MAX_REASON_BYTES: usize = 200;
/// The plaintext's version byte.
pub const PLAINTEXT_VERSION: u8 = 1;

const KX_LABEL: &[u8] = b"brawlonline-chat-kx-v1";
const KEY_LABEL: &[u8] = b"brawlonline-chat-key-v1";
const MSG_LABEL: &[u8] = b"brawlonline-chat-msg-v1";
const BOX_LABEL: &[u8] = b"brawlonline-chat-box-v1";

// ---------------------------------------------------------------------------------------------
// Field checks

/// The characters a group id is made of: `A-Z a-z 0-9 . _ : + -`.
pub fn is_group_id_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b':' | b'+' | b'-')
}

/// A group id: 1 to [`MAX_GROUP_ID_LEN`] characters of [`is_group_id_char`].
pub fn is_group_id(s: &str) -> bool {
    !s.is_empty() && s.len() <= MAX_GROUP_ID_LEN && s.bytes().all(is_group_id_char)
}

/// A uid on the wire: the account's UUID in its 36-character hyphenated lowercase form.
pub fn is_uid(s: &str) -> bool {
    s.len() == 36
        && s.bytes().enumerate().all(|(i, b)| match i {
            8 | 13 | 18 | 23 => b == b'-',
            _ => b.is_ascii_digit() || (b'a'..=b'f').contains(&b),
        })
}

/// Decodes hex (either case) of at most `max_bytes` bytes. `None` for an odd length, a non-hex
/// digit or too many bytes (checked before decoding).
pub fn decode_hex(s: &str, max_bytes: usize) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 || s.len() / 2 > max_bytes {
        return None;
    }
    hex::decode(s).ok()
}

/// Decodes exactly `N` bytes of hex (either case).
pub fn decode_hex_exact<const N: usize>(s: &str) -> Option<[u8; N]> {
    if s.len() != 2 * N {
        return None;
    }
    let mut out = [0u8; N];
    hex::decode_to_slice(s, &mut out).ok()?;
    Some(out)
}

/// An identity key from `hello.chatKey`: 64 hex digits that are a valid Ed25519 public key and
/// not one of the small-order ("weak") points. `None` means the connection can't chat.
pub fn parse_identity_key(s: &str) -> Option<[u8; 32]> {
    let key = decode_hex_exact::<32>(s)?;
    let vk = VerifyingKey::from_bytes(&key).ok()?;
    (!vk.is_weak()).then_some(key)
}

// ---------------------------------------------------------------------------------------------
// Client → server

/// The `op` a `chat-error` names for a request of type `kind`: the type itself for the four
/// requests, `invalid` for anything else (never an echo of what the peer sent).
pub fn op_of(kind: &str) -> &'static str {
    match kind {
        CHAT_KEY => CHAT_KEY,
        CHAT_SEND => CHAT_SEND,
        CHAT_LEAVE => CHAT_LEAVE,
        CHAT_REPORT => CHAT_REPORT,
        _ => "invalid",
    }
}

/// One message of a `chat-report`: as the reporter decrypted it (the text before cleaning, which
/// is what the signature covers).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReportedMessage {
    pub seq: u64,
    pub text: String,
    pub sig: [u8; 64],
}

/// A `chat-*` request, client → server, on an online connection, with every field checked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChatRequest {
    /// `chat-key {group, kx, sig}`: the sender's group key for `group`.
    Key { group: String, kx: [u8; 32], sig: [u8; 64] },
    /// `chat-send {group, boxes}`: one box per recipient uid (sorted by uid).
    Send { group: String, boxes: Vec<(String, Vec<u8>)> },
    /// `chat-leave {group}`.
    Leave { group: String },
    /// `chat-report {group, from, reason, messages}`.
    Report { group: String, from: String, reason: String, messages: Vec<ReportedMessage> },
}

impl ChatRequest {
    /// The message type, for logs and `chat-error.op`.
    pub fn op(&self) -> &'static str {
        match self {
            ChatRequest::Key { .. } => CHAT_KEY,
            ChatRequest::Send { .. } => CHAT_SEND,
            ChatRequest::Leave { .. } => CHAT_LEAVE,
            ChatRequest::Report { .. } => CHAT_REPORT,
        }
    }

    /// The group the request is about.
    pub fn group(&self) -> &str {
        match self {
            ChatRequest::Key { group, .. }
            | ChatRequest::Send { group, .. }
            | ChatRequest::Leave { group }
            | ChatRequest::Report { group, .. } => group,
        }
    }

    /// The wire form (for clients).
    pub fn to_json(&self) -> Value {
        let mut v = match self {
            ChatRequest::Key { group, kx, sig } => {
                json!({"group": group, "kx": hex::encode(kx), "sig": hex::encode(sig)})
            }
            ChatRequest::Send { group, boxes } => {
                let boxes: serde_json::Map<String, Value> =
                    boxes.iter().map(|(uid, b)| (uid.clone(), Value::String(hex::encode(b)))).collect();
                json!({"group": group, "boxes": boxes})
            }
            ChatRequest::Leave { group } => json!({"group": group}),
            ChatRequest::Report { group, from, reason, messages } => json!({
                "group": group,
                "from": from,
                "reason": reason,
                "messages": messages
                    .iter()
                    .map(|m| json!({"seq": m.seq, "text": m.text, "sig": hex::encode(m.sig)}))
                    .collect::<Vec<_>>(),
            }),
        };
        v["type"] = json!(self.op());
        v
    }

    /// Parses a `chat-*` message. `Err` names what is wrong (the server answers `chat-error`).
    pub fn parse(kind: &str, v: &Value) -> Result<ChatRequest, String> {
        let string = |key: &str| -> Result<&str, String> {
            v.get(key).and_then(|s| s.as_str()).ok_or_else(|| format!("{kind}: `{key}` must be a string"))
        };
        let group = || -> Result<String, String> {
            let g = string("group")?;
            if !is_group_id(g) {
                return Err(format!("{kind}: `group` is not a group id"));
            }
            Ok(g.to_string())
        };
        Ok(match kind {
            CHAT_KEY => ChatRequest::Key {
                group: group()?,
                kx: decode_hex_exact::<32>(string("kx")?)
                    .ok_or_else(|| format!("{kind}: `kx` must be 64 hex digits"))?,
                sig: decode_hex_exact::<64>(string("sig")?)
                    .ok_or_else(|| format!("{kind}: `sig` must be 128 hex digits"))?,
            },
            CHAT_SEND => {
                let group = group()?;
                let obj =
                    v.get("boxes").and_then(|b| b.as_object()).ok_or_else(|| format!("{kind}: `boxes` missing"))?;
                if obj.is_empty() {
                    return Err(format!("{kind}: no boxes"));
                }
                if obj.len() >= MAX_GROUP_MEMBERS {
                    return Err(format!("{kind}: more boxes than other members"));
                }
                let mut boxes = Vec::with_capacity(obj.len());
                for (uid, b) in obj {
                    if !is_uid(uid) {
                        return Err(format!("{kind}: a box is not addressed to a uid"));
                    }
                    let sealed = b
                        .as_str()
                        .and_then(|s| decode_hex(s, MAX_BOX_BYTES))
                        .filter(|b| b.len() >= MIN_BOX_BYTES)
                        .ok_or_else(|| {
                            format!("{kind}: a box must be {MIN_BOX_BYTES} to {MAX_BOX_BYTES} bytes of hex")
                        })?;
                    boxes.push((uid.clone(), sealed));
                }
                ChatRequest::Send { group, boxes }
            }
            CHAT_LEAVE => ChatRequest::Leave { group: group()? },
            CHAT_REPORT => {
                let group = group()?;
                let from = string("from")?;
                if !is_uid(from) {
                    return Err(format!("{kind}: `from` is not a uid"));
                }
                let reason = match v.get("reason") {
                    None | Some(Value::Null) => "",
                    Some(r) => r.as_str().ok_or_else(|| format!("{kind}: `reason` must be a string"))?,
                };
                if reason.len() > MAX_REASON_BYTES {
                    return Err(format!("{kind}: `reason` is longer than {MAX_REASON_BYTES} bytes"));
                }
                let list = match v.get("messages") {
                    None | Some(Value::Null) => &[][..],
                    Some(m) => m.as_array().ok_or_else(|| format!("{kind}: `messages` must be a list"))?,
                };
                if list.len() > MAX_REPORT_MESSAGES {
                    return Err(format!("{kind}: more than {MAX_REPORT_MESSAGES} messages"));
                }
                let mut messages = Vec::with_capacity(list.len());
                for m in list {
                    let seq = m.get("seq").and_then(|s| s.as_u64()).ok_or_else(|| format!("{kind}: bad `seq`"))?;
                    let text = m
                        .get("text")
                        .and_then(|t| t.as_str())
                        .filter(|t| !t.is_empty() && t.len() <= MAX_TEXT_BYTES)
                        .ok_or_else(|| format!("{kind}: `text` must be 1 to {MAX_TEXT_BYTES} bytes"))?;
                    let sig = m
                        .get("sig")
                        .and_then(|s| s.as_str())
                        .and_then(decode_hex_exact::<64>)
                        .ok_or_else(|| format!("{kind}: `sig` must be 128 hex digits"))?;
                    messages.push(ReportedMessage { seq, text: text.to_string(), sig });
                }
                ChatRequest::Report { group, from: from.to_string(), reason: reason.to_string(), messages }
            }
            other => return Err(format!("unknown chat message {other:?}")),
        })
    }
}

// ---------------------------------------------------------------------------------------------
// Server → client

/// One member in `chat-group`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatMember {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    /// The member's identity key (hex).
    pub id_key: String,
    /// The member's group key and its signature (hex), null until it sent `chat-key`.
    #[serde(default)]
    pub kx: Option<String>,
    #[serde(default)]
    pub kx_sig: Option<String>,
}

/// `chat-group`, server → every member after every change; `group: null` (and nothing else) to a
/// connection that left its group.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatGroup {
    #[serde(rename = "type")]
    pub kind: String,
    pub group: Option<String>,
    /// `room` or `match`.
    #[serde(rename = "kind", default, skip_serializing_if = "Option::is_none")]
    pub group_kind: Option<String>,
    /// The receiver's uid.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub you: Option<String>,
    /// In joining order, the receiver included.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub members: Option<Vec<ChatMember>>,
}

impl ChatGroup {
    /// `{"type": "chat-group", "group": null}`.
    pub fn none() -> Self {
        ChatGroup { kind: CHAT_GROUP.into(), group: None, group_kind: None, you: None, members: None }
    }
}

/// `chat-msg`, server → one recipient.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatMsg {
    #[serde(rename = "type")]
    pub kind: String,
    pub group: String,
    pub from: String,
    /// The box (hex).
    #[serde(rename = "box")]
    pub sealed: String,
}

/// `chat-reported`, server → the reporter.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatReported {
    #[serde(rename = "type")]
    pub kind: String,
    pub from: String,
}

/// `chat-error`, server → the sender: a request was refused; nothing was forwarded.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ChatError {
    #[serde(rename = "type")]
    pub kind: String,
    pub op: String,
    pub error: String,
}

// ---------------------------------------------------------------------------------------------
// Crypto (`docs/chat-protocol.md` §4)

type Blake2b256 = Blake2b<U32>;

/// `label ‖ 0 ‖ G ‖ 0`, the start of every signed or hashed string.
fn labelled(label: &[u8], group: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(label.len() + group.len() + 2 + 128);
    v.extend_from_slice(label);
    v.push(0);
    v.extend_from_slice(group.as_bytes());
    v.push(0);
    v
}

/// The identity public key of a 32-byte Ed25519 seed.
pub fn identity_public(seed: &[u8; 32]) -> [u8; 32] {
    SigningKey::from_bytes(seed).verifying_key().to_bytes()
}

/// A random 32-byte secret (an identity seed or a group key's secret).
pub fn random_secret() -> [u8; 32] {
    rand::random()
}

fn verify(id_key: &[u8; 32], msg: &[u8], sig: &[u8; 64]) -> bool {
    let Ok(vk) = VerifyingKey::from_bytes(id_key) else { return false };
    vk.verify_strict(msg, &Signature::from_bytes(sig)).is_ok()
}

/// What `kxSig` signs: `"brawlonline-chat-kx-v1" ‖ 0 ‖ G ‖ 0 ‖ kx`.
pub fn kx_signed_bytes(group: &str, kx: &[u8; 32]) -> Vec<u8> {
    let mut v = labelled(KX_LABEL, group);
    v.extend_from_slice(kx);
    v
}

/// `kxSig`: the identity's signature over its group key.
pub fn sign_kx(id_seed: &[u8; 32], group: &str, kx: &[u8; 32]) -> [u8; 64] {
    SigningKey::from_bytes(id_seed).sign(&kx_signed_bytes(group, kx)).to_bytes()
}

pub fn verify_kx(id_key: &[u8; 32], group: &str, kx: &[u8; 32], sig: &[u8; 64]) -> bool {
    verify(id_key, &kx_signed_bytes(group, kx), sig)
}

/// The X25519 public key of a group key's secret.
pub fn kx_public(secret: &[u8; 32]) -> [u8; 32] {
    PublicKey::from(&StaticSecret::from(*secret)).to_bytes()
}

/// The key a pair of members shares in `group`: `s = X25519(own secret, their kx)` (refused when
/// all zero: their key is of small order), then `BLAKE2b-256("brawlonline-chat-key-v1" ‖ 0 ‖ G ‖
/// 0 ‖ s ‖ lo ‖ hi)` with `lo`, `hi` the two public keys in byte order. Both sides get the same.
pub fn pair_key(group: &str, own_secret: &[u8; 32], their_kx: &[u8; 32]) -> Option<[u8; 32]> {
    let secret = StaticSecret::from(*own_secret);
    let own = PublicKey::from(&secret).to_bytes();
    let shared = secret.diffie_hellman(&PublicKey::from(*their_kx));
    if shared.as_bytes().iter().all(|b| *b == 0) {
        return None;
    }
    let (lo, hi) = if own <= *their_kx { (own, *their_kx) } else { (*their_kx, own) };
    let mut h = Blake2b256::new();
    h.update(labelled(KEY_LABEL, group));
    h.update(shared.as_bytes());
    h.update(lo);
    h.update(hi);
    Some(h.finalize().into())
}

/// What a message signature signs: `"brawlonline-chat-msg-v1" ‖ 0 ‖ G ‖ 0 ‖ from ‖ 0 ‖
/// u64le(seq) ‖ text`.
pub fn message_signed_bytes(group: &str, from: &str, seq: u64, text: &[u8]) -> Vec<u8> {
    let mut v = labelled(MSG_LABEL, group);
    v.extend_from_slice(from.as_bytes());
    v.push(0);
    v.extend_from_slice(&seq.to_le_bytes());
    v.extend_from_slice(text);
    v
}

pub fn sign_message(id_seed: &[u8; 32], group: &str, from: &str, seq: u64, text: &[u8]) -> [u8; 64] {
    SigningKey::from_bytes(id_seed).sign(&message_signed_bytes(group, from, seq, text)).to_bytes()
}

pub fn verify_message(id_key: &[u8; 32], group: &str, from: &str, seq: u64, text: &[u8], sig: &[u8; 64]) -> bool {
    verify(id_key, &message_signed_bytes(group, from, seq, text), sig)
}

/// The plaintext of a box: `0x01 ‖ u64le(seq) ‖ sig ‖ text`.
pub fn encode_plaintext(seq: u64, sig: &[u8; 64], text: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(PLAINTEXT_OVERHEAD + text.len());
    v.push(PLAINTEXT_VERSION);
    v.extend_from_slice(&seq.to_le_bytes());
    v.extend_from_slice(sig);
    v.extend_from_slice(text);
    v
}

/// A plaintext's `(seq, sig, text)`; `None` for another version or a text that is empty or longer
/// than [`MAX_TEXT_BYTES`]. The text is not checked further (that is [`clean_text`]'s job).
pub fn parse_plaintext(p: &[u8]) -> Option<(u64, [u8; 64], &[u8])> {
    if p.len() <= PLAINTEXT_OVERHEAD || p.len() > PLAINTEXT_OVERHEAD + MAX_TEXT_BYTES || p[0] != PLAINTEXT_VERSION {
        return None;
    }
    let seq = u64::from_le_bytes(p[1..9].try_into().ok()?);
    let sig: [u8; 64] = p[9..PLAINTEXT_OVERHEAD].try_into().ok()?;
    Some((seq, sig, &p[PLAINTEXT_OVERHEAD..]))
}

/// A box's associated data: `"brawlonline-chat-box-v1" ‖ 0 ‖ G ‖ 0 ‖ from ‖ 0 ‖ to`.
pub fn box_ad(group: &str, from: &str, to: &str) -> Vec<u8> {
    let mut v = labelled(BOX_LABEL, group);
    v.extend_from_slice(from.as_bytes());
    v.push(0);
    v.extend_from_slice(to.as_bytes());
    v
}

/// Seals a plaintext: `nonce(24) ‖ mac(16) ‖ ciphertext`, Monocypher's `crypto_aead_lock` layout
/// (RustCrypto's own output is `ciphertext ‖ tag`).
pub fn seal(key: &[u8; 32], nonce: &[u8; 24], ad: &[u8], plaintext: &[u8]) -> Vec<u8> {
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    let mut ct = plaintext.to_vec();
    // Fails only for plaintexts beyond 256 GiB; ours are a few hundred bytes.
    let tag = cipher.encrypt_in_place_detached(XNonce::from_slice(nonce), ad, &mut ct).expect("plaintext fits");
    let mut out = Vec::with_capacity(24 + 16 + ct.len());
    out.extend_from_slice(nonce);
    out.extend_from_slice(&tag);
    out.extend_from_slice(&ct);
    out
}

/// Opens a box made by [`seal`]; `None` if it is too short or does not authenticate.
pub fn open(key: &[u8; 32], ad: &[u8], sealed: &[u8]) -> Option<Vec<u8>> {
    if sealed.len() < 24 + 16 {
        return None;
    }
    let (nonce, rest) = sealed.split_at(24);
    let (tag, ct) = rest.split_at(16);
    let cipher = XChaCha20Poly1305::new(Key::from_slice(key));
    let mut pt = ct.to_vec();
    cipher.decrypt_in_place_detached(XNonce::from_slice(nonce), ad, &mut pt, Tag::from_slice(tag)).ok()?;
    Some(pt)
}

// ---------------------------------------------------------------------------------------------
// Text (`docs/chat-protocol.md` §5)

/// Characters rule 2 removes: controls, invisible and bidi formatting, variation selectors,
/// fillers, private use, tags, planes 15-16 and noncharacters.
fn is_removed(c: char) -> bool {
    let u = c as u32;
    matches!(
        u,
        0x00..=0x1F
            | 0x7F..=0x9F
            | 0xAD
            | 0x34F
            | 0x61C
            | 0x115F
            | 0x1160
            | 0x17B4
            | 0x17B5
            | 0x180B..=0x180F
            | 0x200B..=0x200F
            | 0x2028..=0x202E
            | 0x2060..=0x206F
            | 0x3164
            | 0xFE00..=0xFE0F
            | 0xFEFF
            | 0xFFA0
            | 0xFFF0..=0xFFFB
            | 0xE000..=0xF8FF
            | 0x1D173..=0x1D17A
            | 0xE0000..=0xE0FFF
            | 0xF0000..=0x10FFFF
            | 0xFDD0..=0xFDEF
    ) || (u & 0xFFFE) == 0xFFFE
}

/// Rule 3's whitespace.
fn is_space(c: char) -> bool {
    matches!(c as u32, 0x20 | 0xA0 | 0x1680 | 0x2000..=0x200A | 0x202F | 0x205F | 0x3000)
}

/// Rule 4's combining marks.
fn is_combining(c: char) -> bool {
    matches!(
        c as u32,
        0x300..=0x36F
            | 0x483..=0x489
            | 0x591..=0x5BD
            | 0x610..=0x61A
            | 0x64B..=0x65F
            | 0xE31..=0xE3A
            | 0xE47..=0xE4E
            | 0x1AB0..=0x1AFF
            | 0x1DC0..=0x1DFF
            | 0x20D0..=0x20FF
            | 0x302A..=0x302F
            | 0x3099
            | 0x309A
            | 0xFE20..=0xFE2F
    )
}

/// The text cleaner both sides apply to a message (and to names, with
/// [`MAX_NAME_CODE_POINTS`]), in the spec's order: invalid UTF-8 is dropped (`None`); controls,
/// invisible, bidi, tag, variation-selector, private-use and noncharacter code points are removed;
/// whitespace runs become one space and leading and trailing spaces go; at most two combining
/// marks in a row stay; the result is cut on a code point boundary to `max_code_points` and
/// [`MAX_TEXT_BYTES`]. `None` if nothing is left.
pub fn clean_text(bytes: &[u8], max_code_points: usize) -> Option<String> {
    let s = std::str::from_utf8(bytes).ok()?;
    let mut cleaned = String::with_capacity(s.len().min(4 * MAX_TEXT_BYTES));
    let mut space = false;
    let mut marks = 0usize;
    for c in s.chars().filter(|c| !is_removed(*c)) {
        if is_space(c) {
            space = true;
            marks = 0;
            continue;
        }
        if is_combining(c) {
            if marks >= 2 {
                continue;
            }
            marks += 1;
        } else {
            marks = 0;
        }
        if space && !cleaned.is_empty() {
            cleaned.push(' ');
        }
        space = false;
        cleaned.push(c);
    }
    let mut out = String::with_capacity(cleaned.len().min(MAX_TEXT_BYTES));
    for c in cleaned.chars().take(max_code_points) {
        if out.len() + c.len_utf8() > MAX_TEXT_BYTES {
            break;
        }
        out.push(c);
    }
    // A space the cut left at the end goes too, so cleaning twice changes nothing.
    if out.ends_with(' ') {
        out.pop();
    }
    (!out.is_empty()).then_some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const GROUP: &str = "room-KFQB-12";
    const SENDER: &str = "11111111-1111-4111-8111-111111111111";
    const RECIPIENT: &str = "22222222-2222-4222-8222-222222222222";
    const TEXT: &str = "hello ｗｏｒｌｄ ねこ";

    fn seq_bytes(start: u8, n: usize) -> Vec<u8> {
        (0..n).map(|i| start.wrapping_add(i as u8)).collect()
    }
    fn arr32(start: u8) -> [u8; 32] {
        seq_bytes(start, 32).try_into().unwrap()
    }

    // Fixed inputs of the test vectors.
    fn sender_id_seed() -> [u8; 32] {
        arr32(0x00)
    }
    fn recipient_id_seed() -> [u8; 32] {
        arr32(0x20)
    }
    fn sender_kx_secret() -> [u8; 32] {
        arr32(0x40)
    }
    fn recipient_kx_secret() -> [u8; 32] {
        arr32(0x60)
    }
    fn nonce() -> [u8; 24] {
        seq_bytes(0x80, 24).try_into().unwrap()
    }

    // Expected outputs (also checked by Dolphin's `ChatCryptoTest.cpp`). Cross-checked against
    // libsodium (PyNaCl 1.5: Ed25519, X25519, BLAKE2b with a 32-byte digest, XChaCha20-Poly1305
    // IETF with the tag moved in front of the ciphertext) when they were made.
    const SENDER_ID_KEY: &str = "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
    const RECIPIENT_ID_KEY: &str = "29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
    const SENDER_KX: &str = "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a";
    const RECIPIENT_KX: &str = "675dd574ed7789310b3d2e7681f3790b466c773b1521fecf36577958371ea52f";
    const SENDER_KX_SIG: &str = "47ec39ad5b0e785b3ad40d3afb9d876439e9cbfc6ccc4c36bc8e0e2778b11581ff51f27cce788697fdf08e404a897a7a4fff7b0b2f084cbfab00288ce939250d";
    const PAIR_KEY: &str = "b58346539534a6603561da2c02479171669efd69a12ccc8114ba6d570ec54fc5";
    const MESSAGE_SIG: &str = "655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a";
    const PLAINTEXT: &str = "010100000000000000655a160d056df1f602c0aa45e60e8c9002956eff9d3ec1f96c65a447d180715e748199ea5b8c356e62a120f1278ecd49dbd038dd5114d1d4f7115c2ac168660a68656c6c6f20efbd97efbd8fefbd92efbd8cefbd8420e381ade38193";
    const AD: &str = "627261776c6f6e6c696e652d636861742d626f782d763100726f6f6d2d4b4651422d31320031313131313131312d313131312d343131312d383131312d3131313131313131313131310032323232323232322d323232322d343232322d383232322d323232323232323232323232";
    const BOX: &str = "808182838485868788898a8b8c8d8e8f90919293949596972006179349a1893a3f0f1207a3f24df6a4210813228f3094a7319f3945640c4c55a3a6b54d78bff9f56e520e248754480102d74ec7034e692b38caddc70c057d50bff23530fa4e1d4f4a563f23b3e9b4eecd2101486f3a1a5a0111b29cbdd0d3168dc69757a22428c3b191ac6cb7289fd9dd2f6722";

    /// Prints a vector for copying into the C++ test (`cargo test -p common test_vectors --
    /// --nocapture`).
    fn show(name: &str, bytes: &[u8]) -> String {
        let h = hex::encode(bytes);
        println!("{name:<16} {h}");
        h
    }

    #[test]
    fn test_vectors() {
        let sender_id = show("sender idKey", &identity_public(&sender_id_seed()));
        let recipient_id = show("recipient idKey", &identity_public(&recipient_id_seed()));
        let skx = kx_public(&sender_kx_secret());
        let rkx = kx_public(&recipient_kx_secret());
        let sender_kx = show("sender kx", &skx);
        let recipient_kx = show("recipient kx", &rkx);
        let kx_sig = sign_kx(&sender_id_seed(), GROUP, &skx);
        let kx_sig_hex = show("sender kxSig", &kx_sig);
        let key = pair_key(GROUP, &sender_kx_secret(), &rkx).unwrap();
        assert_eq!(Some(key), pair_key(GROUP, &recipient_kx_secret(), &skx), "both sides derive one key");
        let key_hex = show("pair key", &key);
        let sig = sign_message(&sender_id_seed(), GROUP, SENDER, 1, TEXT.as_bytes());
        let sig_hex = show("message sig", &sig);
        let pt = encode_plaintext(1, &sig, TEXT.as_bytes());
        let pt_hex = show("plaintext", &pt);
        let ad = box_ad(GROUP, SENDER, RECIPIENT);
        let ad_hex = show("ad", &ad);
        let sealed = seal(&key, &nonce(), &ad, &pt);
        let box_hex = show("box", &sealed);

        assert_eq!(sender_id, SENDER_ID_KEY);
        assert_eq!(recipient_id, RECIPIENT_ID_KEY);
        assert_eq!(sender_kx, SENDER_KX);
        assert_eq!(recipient_kx, RECIPIENT_KX);
        assert_eq!(kx_sig_hex, SENDER_KX_SIG);
        assert_eq!(key_hex, PAIR_KEY);
        assert_eq!(sig_hex, MESSAGE_SIG);
        assert_eq!(pt_hex, PLAINTEXT);
        assert_eq!(ad_hex, AD);
        assert_eq!(box_hex, BOX);

        // And everything checks out on the receiving side.
        let id = decode_hex_exact::<32>(&sender_id).unwrap();
        assert!(verify_kx(&id, GROUP, &skx, &kx_sig));
        let opened = open(&key, &ad, &sealed).unwrap();
        let (seq, s, text) = parse_plaintext(&opened).unwrap();
        assert_eq!((seq, text), (1, TEXT.as_bytes()));
        assert!(verify_message(&id, GROUP, SENDER, seq, text, &s));
        assert_eq!(clean_text(text, MAX_TEXT_CODE_POINTS).as_deref(), Some(TEXT));
    }

    #[test]
    fn seal_open_round_trip_and_tampering() {
        let key = pair_key(GROUP, &sender_kx_secret(), &kx_public(&recipient_kx_secret())).unwrap();
        let ad = box_ad(GROUP, SENDER, RECIPIENT);
        let pt = encode_plaintext(7, &[9; 64], b"gg");
        let sealed = seal(&key, &nonce(), &ad, &pt);
        assert_eq!(sealed.len(), 24 + 16 + pt.len());
        assert_eq!(open(&key, &ad, &sealed).unwrap(), pt);
        // Every flipped bit (nonce, MAC, ciphertext) fails.
        for i in 0..sealed.len() {
            let mut bad = sealed.clone();
            bad[i] ^= 0x01;
            assert!(open(&key, &ad, &bad).is_none(), "byte {i}");
        }
        // Other associated data: another group, the sender and recipient swapped, another recipient.
        for other in [
            box_ad("room-KFQB-13", SENDER, RECIPIENT),
            box_ad(GROUP, RECIPIENT, SENDER),
            box_ad(GROUP, SENDER, "33333333-3333-4333-8333-333333333333"),
        ] {
            assert!(open(&key, &other, &sealed).is_none());
        }
        // The wrong recipient's key (a third member) does not open it.
        let third = kx_public(&arr32(0xA0));
        let wrong = pair_key(GROUP, &sender_kx_secret(), &third).unwrap();
        assert!(open(&wrong, &ad, &sealed).is_none());
        // The same pair in another group has another key.
        assert_ne!(pair_key("room-KFQB-13", &sender_kx_secret(), &kx_public(&recipient_kx_secret())).unwrap(), key);
        // Truncated boxes.
        for n in [0, 1, 39, 40] {
            assert!(open(&key, &ad, &sealed[..n]).is_none());
        }
        // A replay opens fine: dropping it (seq not above the last) is the receiver's job.
        assert_eq!(open(&key, &ad, &sealed).unwrap(), pt);
    }

    #[test]
    fn small_order_keys_are_refused() {
        // X25519: the all-zero point (and other small-order points) give an all-zero secret.
        assert_eq!(pair_key(GROUP, &sender_kx_secret(), &[0; 32]), None);
        let mut one = [0u8; 32];
        one[0] = 1;
        assert_eq!(pair_key(GROUP, &sender_kx_secret(), &one), None);
        // Ed25519: a weak identity key is not a chat key; neither is a non-point.
        assert_eq!(parse_identity_key(&hex::encode(one)), None);
        let not_a_point = (2u8..=255)
            .map(|y| {
                let mut k = [0u8; 32];
                k[0] = y;
                k
            })
            .find(|k| VerifyingKey::from_bytes(k).is_err())
            .unwrap();
        assert_eq!(parse_identity_key(&hex::encode(not_a_point)), None);
        let good = identity_public(&sender_id_seed());
        assert_eq!(parse_identity_key(&hex::encode(good)), Some(good));
        assert_eq!(parse_identity_key(&hex::encode(good).to_uppercase()), Some(good));
        assert_eq!(parse_identity_key(&hex::encode(good)[..62]), None);
        assert_eq!(parse_identity_key(""), None);
    }

    #[test]
    fn signatures_bind_every_field() {
        let seed = sender_id_seed();
        let id = identity_public(&seed);
        let sig = sign_message(&seed, GROUP, SENDER, 3, b"hi");
        assert!(verify_message(&id, GROUP, SENDER, 3, b"hi", &sig));
        assert!(!verify_message(&id, GROUP, SENDER, 4, b"hi", &sig));
        assert!(!verify_message(&id, GROUP, SENDER, 3, b"ho", &sig));
        assert!(!verify_message(&id, "room-KFQB-1", SENDER, 3, b"hi", &sig));
        assert!(!verify_message(&id, GROUP, RECIPIENT, 3, b"hi", &sig));
        assert!(!verify_message(&identity_public(&recipient_id_seed()), GROUP, SENDER, 3, b"hi", &sig));
        let kx = kx_public(&sender_kx_secret());
        let ks = sign_kx(&seed, GROUP, &kx);
        assert!(verify_kx(&id, GROUP, &kx, &ks));
        assert!(!verify_kx(&id, "room-KFQB-1", &kx, &ks));
        assert!(!verify_kx(&id, GROUP, &kx_public(&recipient_kx_secret()), &ks));
        // A message signature is not a group key signature (the labels differ).
        assert!(!verify_kx(&id, GROUP, &kx, &sig));
        assert!(!verify_message(&[0; 32], GROUP, SENDER, 3, b"hi", &sig));
    }

    #[test]
    fn plaintext_parsing() {
        let p = encode_plaintext(u64::MAX, &[7; 64], b"x");
        assert_eq!(parse_plaintext(&p), Some((u64::MAX, [7; 64], &b"x"[..])));
        assert_eq!(parse_plaintext(&encode_plaintext(1, &[7; 64], b"")), None);
        assert!(parse_plaintext(&encode_plaintext(1, &[7; 64], &[b'a'; MAX_TEXT_BYTES])).is_some());
        assert_eq!(parse_plaintext(&encode_plaintext(1, &[7; 64], &[b'a'; MAX_TEXT_BYTES + 1])), None);
        let mut v2 = p.clone();
        v2[0] = 2;
        assert_eq!(parse_plaintext(&v2), None);
        assert_eq!(parse_plaintext(&[]), None);
        assert_eq!(parse_plaintext(&p[..PLAINTEXT_OVERHEAD]), None);
    }

    #[test]
    fn field_checks() {
        assert!(is_uid(SENDER));
        for bad in [
            "",
            "11111111-1111-4111-8111-11111111111",
            "11111111-1111-4111-8111-1111111111111",
            "11111111-1111-4111-8111-11111111111A",
            "111111111-111-4111-8111-111111111111",
            "11111111_1111_4111_8111_111111111111",
            "1111111g-1111-4111-8111-111111111111",
        ] {
            assert!(!is_uid(bad), "{bad:?}");
        }
        assert!(is_group_id(GROUP));
        assert!(is_group_id("mode.direct-2026-10-09T15:00:00.000Z-1f"));
        assert!(is_group_id("a+b_c"));
        assert!(is_group_id(&"a".repeat(MAX_GROUP_ID_LEN)));
        for bad in ["", "room KFQB", "room/KFQB", "ルーム", "a\0b"] {
            assert!(!is_group_id(bad), "{bad:?}");
        }
        assert!(!is_group_id(&"a".repeat(MAX_GROUP_ID_LEN + 1)));
        assert_eq!(decode_hex("0aFF", 2), Some(vec![0x0a, 0xff]));
        assert_eq!(decode_hex("0aff00", 2), None);
        assert_eq!(decode_hex("0af", 2), None);
        assert_eq!(decode_hex("zz", 2), None);
        assert_eq!(decode_hex("", 2), Some(vec![]));
        assert_eq!(decode_hex_exact::<2>("0102"), Some([1, 2]));
        assert_eq!(decode_hex_exact::<2>("01"), None);
        assert_eq!(decode_hex_exact::<2>("01g2"), None);
    }

    #[test]
    fn requests_parse_and_round_trip() {
        let cases = [
            ChatRequest::Key { group: GROUP.into(), kx: [1; 32], sig: [2; 64] },
            ChatRequest::Send {
                group: GROUP.into(),
                boxes: vec![(SENDER.into(), vec![3; MIN_BOX_BYTES]), (RECIPIENT.into(), vec![4; MAX_BOX_BYTES])],
            },
            ChatRequest::Leave { group: "mode.ranked-x-1".into() },
            ChatRequest::Report {
                group: GROUP.into(),
                from: SENDER.into(),
                reason: "rude".into(),
                messages: vec![ReportedMessage { seq: 7, text: "ねこ".into(), sig: [5; 64] }],
            },
            ChatRequest::Report { group: GROUP.into(), from: SENDER.into(), reason: String::new(), messages: vec![] },
        ];
        for c in cases {
            let v = c.to_json();
            assert_eq!(ChatRequest::parse(v["type"].as_str().unwrap(), &v).unwrap(), c, "{v}");
        }
        let v = json!({"type": "chat-report", "group": GROUP, "from": SENDER});
        assert!(matches!(ChatRequest::parse(CHAT_REPORT, &v), Ok(ChatRequest::Report { .. })));
    }

    #[test]
    fn bad_requests_are_errors() {
        let b = hex::encode([0u8; MIN_BOX_BYTES]);
        let sig = hex::encode([0u8; 64]);
        let msg = |text: Value, sig: &str| json!({"seq": 1, "text": text, "sig": sig});
        let bad = [
            (CHAT_KEY, json!({"group": GROUP, "kx": "00".repeat(31), "sig": sig})),
            (CHAT_KEY, json!({"group": GROUP, "kx": "zz".repeat(32), "sig": sig})),
            (CHAT_KEY, json!({"group": GROUP, "kx": "00".repeat(32), "sig": "00".repeat(63)})),
            (CHAT_KEY, json!({"group": "bad group", "kx": "00".repeat(32), "sig": sig})),
            (CHAT_KEY, json!({"group": 5, "kx": "00".repeat(32), "sig": sig})),
            (CHAT_SEND, json!({"group": GROUP})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {}})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": []})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {"bob": b}})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {SENDER: 5}})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {SENDER: "xyz"}})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {SENDER: hex::encode([0u8; MIN_BOX_BYTES - 1])}})),
            (CHAT_SEND, json!({"group": GROUP, "boxes": {SENDER: hex::encode([0u8; MAX_BOX_BYTES + 1])}})),
            (
                CHAT_SEND,
                json!({"group": GROUP, "boxes": {
                    "11111111-1111-4111-8111-111111111111": b, "22222222-2222-4222-8222-222222222222": b,
                    "33333333-3333-4333-8333-333333333333": b, "44444444-4444-4444-8444-444444444444": b}}),
            ),
            (CHAT_LEAVE, json!({})),
            (CHAT_LEAVE, json!({"group": "x".repeat(97)})),
            (CHAT_REPORT, json!({"group": GROUP, "from": "bob"})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "reason": "x".repeat(201)})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "reason": 5})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": {}})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": vec![msg(json!("a"), &sig); 21]})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": [msg(json!(""), &sig)]})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": [msg(json!("a".repeat(301)), &sig)]})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": [msg(json!(5), &sig)]})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": [msg(json!("a"), "00")]})),
            (CHAT_REPORT, json!({"group": GROUP, "from": SENDER, "messages": [{"seq": -1, "text": "a", "sig": sig}]})),
            ("chat-bogus", json!({"group": GROUP})),
        ];
        for (kind, v) in bad {
            assert!(ChatRequest::parse(kind, &v).is_err(), "{kind} {v}");
        }
    }

    #[test]
    fn server_messages_wire_format() {
        let none = serde_json::to_value(ChatGroup::none()).unwrap();
        assert_eq!(none, json!({"type": "chat-group", "group": null}));
        let g = ChatGroup {
            kind: CHAT_GROUP.into(),
            group: Some(GROUP.into()),
            group_kind: Some("room".into()),
            you: Some(SENDER.into()),
            members: Some(vec![ChatMember {
                uid: SENDER.into(),
                display_name: "alice".into(),
                connect_code: "ALIC#4".into(),
                id_key: "00".repeat(32),
                kx: None,
                kx_sig: None,
            }]),
        };
        let v = serde_json::to_value(&g).unwrap();
        assert_eq!(v["kind"], "room");
        assert_eq!(v["members"][0]["idKey"], "00".repeat(32));
        assert!(v["members"][0]["kx"].is_null() && v["members"][0].get("kxSig").is_some());
        assert_eq!(serde_json::from_value::<ChatGroup>(v).unwrap(), g);
        let m = ChatMsg { kind: CHAT_MSG.into(), group: GROUP.into(), from: SENDER.into(), sealed: "ab".into() };
        assert_eq!(
            serde_json::to_value(m).unwrap(),
            json!({"type": "chat-msg", "group": GROUP, "from": SENDER, "box": "ab"})
        );
    }

    fn clean(s: &str) -> Option<String> {
        clean_text(s.as_bytes(), MAX_TEXT_CODE_POINTS)
    }

    #[test]
    fn clean_text_drops_invalid_utf8() {
        let cases: [&[u8]; 7] = [
            b"\xc0\xaf",         // overlong '/'
            b"\xe0\x80\xaf",     // overlong
            b"\xed\xa0\x80",     // surrogate U+D800
            b"\xf4\x90\x80\x80", // above U+10FFFF
            b"ok \xe3\x81",      // truncated
            b"\xff",             // never valid
            b"a\x80b",           // stray continuation byte
        ];
        for bad in cases {
            assert_eq!(clean_text(bad, MAX_TEXT_CODE_POINTS), None, "{bad:?}");
        }
    }

    #[test]
    fn clean_text_removes_invisible_and_control_characters() {
        // C0, DEL, C1 (tabs and newlines are controls: removed, not spaces).
        assert_eq!(clean("a\u{0}b\u{7}c\td\ne\u{7f}f\u{80}g\u{9f}h").as_deref(), Some("abcdefgh"));
        // Soft hyphen, CGJ, ALM, Hangul fillers, Khmer, Mongolian selectors.
        assert_eq!(
            clean("a\u{ad}\u{34f}\u{61c}\u{115f}\u{1160}\u{17b4}\u{17b5}\u{180b}\u{180f}b").as_deref(),
            Some("ab")
        );
        // Zero width, bidi marks and overrides, line/paragraph separators, word joiner and friends.
        let invisible: String =
            (0x200B..=0x200F).chain(0x2028..=0x202E).chain(0x2060..=0x206F).filter_map(char::from_u32).collect();
        assert_eq!(clean(&format!("x{invisible}y")).as_deref(), Some("xy"));
        // Hangul filler, variation selectors, BOM, halfwidth filler, specials.
        let more: String = [0x3164, 0xFE00, 0xFE0F, 0xFEFF, 0xFFA0, 0xFFF0, 0xFFF9, 0xFFFB]
            .into_iter()
            .filter_map(char::from_u32)
            .collect();
        assert_eq!(clean(&format!("x{more}y")).as_deref(), Some("xy"));
        // Private use, musical formatting, tags, planes 15-16.
        let pua: String =
            [0xE000, 0xF8FF, 0x1D173, 0x1D17A, 0xE0001, 0xE0041, 0xE007F, 0xE0100, 0xE0FFF, 0xF0000, 0x10FFFD]
                .into_iter()
                .filter_map(char::from_u32)
                .collect();
        assert_eq!(clean(&format!("x{pua}y")).as_deref(), Some("xy"));
        // Noncharacters.
        let nonchars: String = [0xFDD0, 0xFDEF, 0xFFFE, 0xFFFF, 0x1FFFE, 0x1FFFF, 0x10FFFF]
            .into_iter()
            .filter_map(char::from_u32)
            .collect();
        assert_eq!(clean(&format!("x{nonchars}y")).as_deref(), Some("xy"));
        // Neighbours of removed ranges stay.
        let kept: String = [0x20A, 0xA1, 0xAC, 0xAE, 0x2027, 0x3163, 0xFDCF, 0xFDF0, 0xFFFD, 0x1D172, 0x1D17B, 0xDFFFD]
            .into_iter()
            .filter_map(char::from_u32)
            .filter(|c| !c.is_control())
            .collect();
        assert_eq!(clean(&kept).as_deref(), Some(kept.as_str()));
        // Only invisible: nothing left.
        assert_eq!(clean("\u{200b}\u{feff}\u{202e}"), None);
        assert_eq!(clean(""), None);
    }

    #[test]
    fn clean_text_collapses_whitespace() {
        assert_eq!(clean("  a \u{a0}\u{3000} b\u{2003}\u{2003}c   ").as_deref(), Some("a b c"));
        let spaces: String = [0x20, 0xA0, 0x1680, 0x2000, 0x200A, 0x202F, 0x205F, 0x3000]
            .into_iter()
            .filter_map(char::from_u32)
            .collect();
        assert_eq!(clean(&format!("a{spaces}b")).as_deref(), Some("a b"));
        assert_eq!(clean(&spaces), None);
        // A removed character between spaces does not keep them apart.
        assert_eq!(clean("a \u{200b} b").as_deref(), Some("a b"));
        assert_eq!(clean("\u{200b} a").as_deref(), Some("a"));
    }

    #[test]
    fn clean_text_limits_combining_marks() {
        assert_eq!(clean("e\u{301}\u{302}\u{303}\u{304}").as_deref(), Some("e\u{301}\u{302}"));
        // A removed character does not break a run; any other character does.
        assert_eq!(clean("e\u{301}\u{200b}\u{302}\u{303}").as_deref(), Some("e\u{301}\u{302}"));
        assert_eq!(clean("e\u{301}\u{302}x\u{303}\u{304}\u{305}").as_deref(), Some("e\u{301}\u{302}x\u{303}\u{304}"));
        assert_eq!(clean("e\u{301}\u{302} \u{303}\u{304}\u{305}").as_deref(), Some("e\u{301}\u{302} \u{303}\u{304}"));
        // Every listed block counts (two kept, the third dropped).
        for (a, b, c) in [
            (0x300, 0x36F, 0x301),
            (0x483, 0x489, 0x484),
            (0x591, 0x5BD, 0x592),
            (0x610, 0x61A, 0x611),
            (0x64B, 0x65F, 0x64C),
            (0xE31, 0xE3A, 0xE34),
            (0xE47, 0xE4E, 0xE48),
            (0x1AB0, 0x1AFF, 0x1AB1),
            (0x1DC0, 0x1DFF, 0x1DC1),
            (0x20D0, 0x20FF, 0x20D1),
            (0x302A, 0x302F, 0x302B),
            (0x3099, 0x309A, 0x3099),
            (0xFE20, 0xFE2F, 0xFE21),
        ] {
            let [a, b, c] = [a, b, c].map(|u| char::from_u32(u).unwrap());
            assert_eq!(clean(&format!("x{a}{b}{c}")), Some(format!("x{a}{b}")), "{:x}", a as u32);
        }
        // Zalgo.
        let zalgo: String = std::iter::once('Z').chain(std::iter::repeat_n('\u{336}', 200)).collect();
        assert_eq!(clean(&zalgo).as_deref(), Some("Z\u{336}\u{336}"));
    }

    #[test]
    fn clean_text_cuts_on_code_point_boundaries() {
        let ascii = "a".repeat(150);
        assert_eq!(clean(&ascii), Some("a".repeat(100)));
        assert_eq!(clean_text(ascii.as_bytes(), MAX_NAME_CODE_POINTS), Some("a".repeat(32)));
        // 100 kana are 300 bytes: all fit. Four-byte characters hit the byte limit first: 75.
        let kana = "ね".repeat(120);
        assert_eq!(clean(&kana), Some("ね".repeat(100)));
        let emoji = "😀".repeat(100);
        assert_eq!(clean(&emoji), Some("😀".repeat(75)));
        // A cut never splits a character: 299 bytes of ASCII and then a 3-byte character.
        let mixed = format!("{}ね", "a".repeat(99));
        assert_eq!(clean(&mixed), Some(mixed.clone()));
        let s = format!("{}{}", "😀".repeat(74), "ねね");
        assert_eq!(clean(&s), Some(format!("{}ね", "😀".repeat(74)))); // 296 + 3 bytes; another 3 is over
        let s = format!("{}ab", "😀".repeat(74));
        assert_eq!(clean(&s), Some(format!("{}ab", "😀".repeat(74))));
        // Counted after cleaning: removed characters and collapsed spaces do not count.
        let padded = format!("{}{}", "\u{200b}".repeat(500), "b".repeat(100));
        assert_eq!(clean(&padded), Some("b".repeat(100)));
        // A space the cut leaves at the end goes too (cleaning twice changes nothing).
        let s = format!("{} b", "a".repeat(99));
        assert_eq!(clean(&s), Some("a".repeat(99)));
        assert_eq!(clean(TEXT).as_deref(), Some(TEXT));
    }
}
