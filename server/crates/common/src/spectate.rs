//! Spectating: the messages and the replay-chunk checks shared by mm, accounts and `mmclient`
//! (`docs/spectate-protocol.md`, design in `docs/design/spectating.md`).
//!
//! A game uploads its replay's chunks exactly as the `.brep` file stores them
//! (`dolphin/Source/Core/Core/Rollback/ReplayFile.h`): a 20-byte header (FourCC, flags, stored
//! length, raw length, CRC-32 of the stored bytes), then the stored bytes. Everything here is
//! checked with hard limits: uploads come from players' games and are untrusted.

use std::io::Read;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const SPECTATE_OFFER: &str = "spectate-offer";
pub const SPECTATE_ROLE: &str = "spectate-role";
pub const SPECTATE_DIGEST: &str = "spectate-digest";
pub const SPECTATE_PAUSE: &str = "spectate-pause";
pub const SPECTATE_RESUME: &str = "spectate-resume";
pub const SPECTATE_STOP: &str = "spectate-stop";
pub const SPECTATE_END: &str = "spectate-end";
pub const SPECTATE_ERROR: &str = "spectate-error";

/// Chunk data travels on this ENet channel (JSON stays on channel 0).
pub const DATA_CHANNEL: u8 = 1;
/// A room's game going to the room's members who are not in it (2026-10-19, `docs/rooms-protocol.md`
/// §2 `room-watch`): mm → game, reliable, binary. Each packet is one byte of kind, then:
/// [`WATCH_BEGIN`] UTF-8 JSON `{matchId, game, number, uid}` (a game of the match starts; its
/// chunks follow), [`WATCH_CHUNK`] the chunk's index in its game (u32, little-endian) and the
/// chunk as uploaded (its 20-byte header included), [`WATCH_END`] UTF-8 JSON `{matchId, game,
/// bad?}` (the game's ENDG came, or `bad`: nothing more of it comes).
pub const WATCH_CHANNEL: u8 = 2;
pub const WATCH_BEGIN: u8 = 1;
pub const WATCH_CHUNK: u8 = 2;
pub const WATCH_END: u8 = 3;
/// `slot`, `seq`, `offset`, `total`.
pub const DATA_HEADER: usize = 16;
/// The largest data packet a game sends (header included).
pub const MAX_DATA_PACKET: usize = 1200;
/// The largest chunk an upload may carry (its 20-byte header included).
pub const MAX_UPLOAD_CHUNK: usize = 64 * 1024 + CHUNK_HEADER;

pub const CHUNK_HEADER: usize = 20;
pub const FLAG_DEFLATE: u32 = 1;
pub const FLAG_CRITICAL: u32 = 2;
pub const PAD_SIZE: usize = 0x40;
pub const FRAMES_PER_CHUNK: usize = 60;
/// A `FRMS` chunk's checksum parts: frame, three RNGs, fighters CRC.
const PARTS_SIZE: usize = 20;

pub const fn fourcc(s: &[u8; 4]) -> u32 {
    u32::from_le_bytes(*s)
}

/// The chunk types an upload may carry, with each one's largest raw size (`ReplayFile.h`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ChunkKind {
    Info,
    Setp,
    Sync,
    Frms,
    Evnt,
    Endg,
}

impl ChunkKind {
    pub fn from_fourcc(v: u32) -> Option<ChunkKind> {
        Some(match &v.to_le_bytes() {
            b"INFO" => ChunkKind::Info,
            b"SETP" => ChunkKind::Setp,
            b"SYNC" => ChunkKind::Sync,
            b"FRMS" => ChunkKind::Frms,
            b"EVNT" => ChunkKind::Evnt,
            b"ENDG" => ChunkKind::Endg,
            _ => return None,
        })
    }

    pub fn name(self) -> &'static str {
        match self {
            ChunkKind::Info => "INFO",
            ChunkKind::Setp => "SETP",
            ChunkKind::Sync => "SYNC",
            ChunkKind::Frms => "FRMS",
            ChunkKind::Evnt => "EVNT",
            ChunkKind::Endg => "ENDG",
        }
    }

    /// `ReplayFile.h`'s `MAX_*` for the raw bytes. `SYNC` has no limit of its own there (it is a
    /// small binary block); 64 KiB is far above any real one.
    pub fn max_raw(self) -> usize {
        match self {
            ChunkKind::Info => 64 << 10,
            ChunkKind::Setp => 256 << 10,
            ChunkKind::Sync => 64 << 10,
            ChunkKind::Frms => 1 << 20,
            ChunkKind::Evnt => 64 << 10,
            ChunkKind::Endg => 16 << 10,
        }
    }

    /// The chunks a game sends before its frames, once each.
    pub fn is_header(self) -> bool {
        matches!(self, ChunkKind::Info | ChunkKind::Setp | ChunkKind::Sync)
    }
}

/// One whole chunk, checked: header, CRC, sizes.
#[derive(Debug, Clone, PartialEq)]
pub struct ChunkView<'a> {
    pub kind: ChunkKind,
    pub flags: u32,
    pub raw_len: usize,
    pub stored: &'a [u8],
}

impl ChunkView<'_> {
    pub fn deflated(&self) -> bool {
        self.flags & FLAG_DEFLATE != 0
    }

    /// The raw bytes: inflated (zlib, as `compress2` writes them) to exactly `raw_len`, or the
    /// stored bytes as they are. Bounded: a stream that would give more than `raw_len` is an
    /// error, never a bigger buffer.
    pub fn raw(&self) -> Result<Vec<u8>, String> {
        if !self.deflated() {
            return Ok(self.stored.to_vec());
        }
        let mut out = Vec::with_capacity(self.raw_len);
        let mut z = flate2::read::ZlibDecoder::new(self.stored).take(self.raw_len as u64 + 1);
        z.read_to_end(&mut out).map_err(|e| format!("{} does not inflate: {e}", self.kind.name()))?;
        if out.len() != self.raw_len {
            return Err(format!("{} inflates to {} bytes, not {}", self.kind.name(), out.len(), self.raw_len));
        }
        Ok(out)
    }
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

/// Checks one whole chunk (header and stored bytes, nothing after them).
pub fn parse_chunk(bytes: &[u8]) -> Result<ChunkView<'_>, String> {
    if bytes.len() < CHUNK_HEADER {
        return Err("chunk shorter than its header".into());
    }
    if bytes.len() > MAX_UPLOAD_CHUNK {
        return Err("chunk too big".into());
    }
    let kind = ChunkKind::from_fourcc(u32_at(bytes, 0)).ok_or("chunk type not allowed")?;
    let flags = u32_at(bytes, 4);
    if flags & !(FLAG_DEFLATE | FLAG_CRITICAL) != 0 {
        return Err(format!("{} has unknown flags {flags:#x}", kind.name()));
    }
    let stored_len = u32_at(bytes, 8) as usize;
    let raw_len = u32_at(bytes, 12) as usize;
    let crc = u32_at(bytes, 16);
    let stored = &bytes[CHUNK_HEADER..];
    if stored.len() != stored_len {
        return Err(format!("{} says {stored_len} stored bytes, has {}", kind.name(), stored.len()));
    }
    if raw_len > kind.max_raw() {
        return Err(format!("{} raw length {raw_len} over its limit", kind.name()));
    }
    if flags & FLAG_DEFLATE == 0 && raw_len != stored_len {
        return Err(format!("{} is not compressed but its lengths differ", kind.name()));
    }
    if crc32fast::hash(stored) != crc {
        return Err(format!("{} CRC mismatch", kind.name()));
    }
    Ok(ChunkView { kind, flags, raw_len, stored })
}

/// A chunk as the file stores it (for tests and `mmclient`): header and stored bytes, compressed
/// when `deflate`.
pub fn encode_chunk(kind: &[u8; 4], raw: &[u8], deflate: bool) -> Vec<u8> {
    let (flags, stored) = if deflate {
        let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
        std::io::Write::write_all(&mut z, raw).expect("writing to memory");
        (FLAG_DEFLATE, z.finish().expect("writing to memory"))
    } else {
        (0, raw.to_vec())
    };
    let mut out = Vec::with_capacity(CHUNK_HEADER + stored.len());
    out.extend_from_slice(kind);
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&(stored.len() as u32).to_le_bytes());
    out.extend_from_slice(&(raw.len() as u32).to_le_bytes());
    out.extend_from_slice(&crc32fast::hash(&stored).to_le_bytes());
    out.extend_from_slice(&stored);
    out
}

/// The start of a raw `FRMS` chunk (`EncodeFrames`): which frames it holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FramesHead {
    pub first_frame: u32,
    pub count: u32,
    pub ports: u8,
}

/// Reads a raw `FRMS` chunk's head and checks its size against it: `u32 first_frame`,
/// `u16 count`, `u8 ports`, a zero byte, `count × 0x40` pad bytes per port in `ports`,
/// `count × u32` checksums, `u16` parts and 20 bytes per part.
pub fn frames_head(raw: &[u8]) -> Result<FramesHead, String> {
    if raw.len() < 8 {
        return Err("FRMS too short".into());
    }
    let first_frame = u32_at(raw, 0);
    let count = u16_at(raw, 4) as usize;
    let ports = raw[6];
    if ports & !0xF != 0 || ports == 0 {
        return Err(format!("FRMS ports {ports:#x}"));
    }
    if count == 0 || count > FRAMES_PER_CHUNK {
        return Err(format!("FRMS holds {count} frames"));
    }
    let pads = ports.count_ones() as usize * count * PAD_SIZE;
    let parts_at = 8 + pads + count * 4;
    if raw.len() < parts_at + 2 {
        return Err("FRMS shorter than its frames".into());
    }
    let parts = u16_at(raw, parts_at) as usize;
    if raw.len() != parts_at + 2 + parts * PARTS_SIZE {
        return Err("FRMS size does not match its frames".into());
    }
    Ok(FramesHead { first_frame, count: count as u32, ports })
}

/// A witness's digest of a raw `FRMS` chunk: the first 8 bytes of its SHA-256.
pub fn frames_digest(raw: &[u8]) -> [u8; 8] {
    let d = Sha256::digest(raw);
    let mut out = [0u8; 8];
    out.copy_from_slice(&d[..8]);
    out
}

/// A data packet's header (channel 1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DataHeader {
    pub slot: u32,
    pub seq: u32,
    pub offset: u32,
    pub total: u32,
}

/// Splits a data packet into its header and bytes. `None`: too short, too long, or bytes that
/// do not fit inside `total`.
pub fn parse_data(packet: &[u8]) -> Option<(DataHeader, &[u8])> {
    if packet.len() <= DATA_HEADER || packet.len() > MAX_DATA_PACKET {
        return None;
    }
    let h = DataHeader {
        slot: u32_at(packet, 0),
        seq: u32_at(packet, 4),
        offset: u32_at(packet, 8),
        total: u32_at(packet, 12),
    };
    let bytes = &packet[DATA_HEADER..];
    let end = (h.offset as usize).checked_add(bytes.len())?;
    if h.total as usize > MAX_UPLOAD_CHUNK || end > h.total as usize {
        return None;
    }
    Some((h, bytes))
}

/// The data packets of one chunk, as a game sends them.
pub fn data_packets(slot: u32, seq: u32, chunk: &[u8]) -> Vec<Vec<u8>> {
    let room = MAX_DATA_PACKET - DATA_HEADER;
    chunk
        .chunks(room)
        .enumerate()
        .map(|(i, part)| {
            let mut p = Vec::with_capacity(DATA_HEADER + part.len());
            p.extend_from_slice(&slot.to_le_bytes());
            p.extend_from_slice(&seq.to_le_bytes());
            p.extend_from_slice(&((i * room) as u32).to_le_bytes());
            p.extend_from_slice(&(chunk.len() as u32).to_le_bytes());
            p.extend_from_slice(part);
            p
        })
        .collect()
}

// ---------------------------------------------------------------- game ↔ mm (channel 0, JSON)

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Offer {
    pub match_id: String,
    pub game_uid: String,
    pub game: u32,
    #[serde(default)]
    pub paused: u32,
    #[serde(default)]
    pub rollbacks: f64,
    /// Whether this player lets others watch their games ("Let others stream my games"). A Ranked
    /// game is offered either way, for the server's record of the set; it is listed only when a
    /// player of it lets others watch. Older builds offer only then: true.
    #[serde(default = "yes")]
    pub listed: bool,
}

fn yes() -> bool {
    true
}

/// `spectate-end`: how a game this player offered ended, from its own `ENDG`. Every player's game
/// sends it, whatever its role: the upload, which carries the `ENDG` too, may have been cut before
/// it (players first), and the list's score would miss that game.
#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EndReport {
    pub match_id: String,
    pub game_uid: String,
    pub end_method: String,
    /// 1-4; none for a draw or no winner.
    #[serde(default)]
    pub winner_port: Option<u32>,
    #[serde(default)]
    pub quitter_port: Option<u32>,
    #[serde(default)]
    pub is_teams: bool,
    /// The game's players as its `INFO` names them.
    #[serde(default)]
    pub players: Vec<EndPlayer>,
    /// Ranked: the set's game, as the game's result report numbers it (`gameIndex`: a desynced
    /// game played again keeps its number, unlike the stream's game number). Older builds: none.
    #[serde(default)]
    pub set_game: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EndPlayer {
    pub port: u32,
    pub uid: String,
    #[serde(default)]
    pub team_id: Option<u32>,
}

/// `ENDG`'s `endMethod`s (`ReplayRecorder.h` EndInfo).
const END_METHODS: [&str; 8] = ["game", "quit", "runback", "quit-left", "disconnect", "desync", "stopped", "error"];

#[derive(Debug, Clone, PartialEq)]
pub enum SpectateRequest {
    Offer(Offer),
    End(EndReport),
    Digest { slot: u32, frms: Vec<(u32, [u8; 8])> },
    Pause { slot: u32, reason: String },
    Resume { slot: u32 },
    Stop { slot: u32, reason: String },
}

/// The most digests one `spectate-digest` may carry.
pub const MAX_DIGESTS: usize = 30;
const REASONS: [&str; 7] = ["frame", "timesync", "rollback", "rtt", "loss", "queue", "other"];

impl SpectateRequest {
    pub fn op(&self) -> &'static str {
        match self {
            SpectateRequest::Offer(_) => SPECTATE_OFFER,
            SpectateRequest::End(_) => SPECTATE_END,
            SpectateRequest::Digest { .. } => SPECTATE_DIGEST,
            SpectateRequest::Pause { .. } => SPECTATE_PAUSE,
            SpectateRequest::Resume { .. } => SPECTATE_RESUME,
            SpectateRequest::Stop { .. } => SPECTATE_STOP,
        }
    }

    /// Reads a `spectate-*` message. The error names the `op` to answer in `spectate-error`.
    pub fn parse(kind: &str, v: &serde_json::Value) -> Result<SpectateRequest, (&'static str, String)> {
        let op = op_of(kind);
        let slot = || v.get("slot").and_then(|s| s.as_u64()).and_then(|s| u32::try_from(s).ok());
        let reason = || -> Result<String, (&'static str, String)> {
            let r = v.get("reason").and_then(|r| r.as_str()).unwrap_or("other");
            if REASONS.contains(&r) {
                Ok(r.to_string())
            } else {
                Err((op, format!("unknown reason {r:?}")))
            }
        };
        match kind {
            SPECTATE_OFFER => {
                let o: Offer = serde_json::from_value(v.clone()).map_err(|e| (op, e.to_string()))?;
                if o.match_id.is_empty() || o.match_id.len() > 96 {
                    return Err((op, "bad matchId".into()));
                }
                if decode_uid(&o.game_uid).is_none() {
                    return Err((op, "bad gameUid".into()));
                }
                if !(1..=99).contains(&o.game) || !o.rollbacks.is_finite() || o.rollbacks < 0.0 {
                    return Err((op, "bad game or rollbacks".into()));
                }
                Ok(SpectateRequest::Offer(o))
            }
            SPECTATE_END => {
                let e: EndReport = serde_json::from_value(v.clone()).map_err(|e| (op, e.to_string()))?;
                if e.match_id.is_empty() || e.match_id.len() > 96 || decode_uid(&e.game_uid).is_none() {
                    return Err((op, "bad matchId or gameUid".into()));
                }
                if !END_METHODS.contains(&e.end_method.as_str()) {
                    return Err((op, format!("unknown endMethod {:?}", e.end_method)));
                }
                if e.set_game.is_some_and(|g| !(1..=99).contains(&g)) {
                    return Err((op, "bad setGame".into()));
                }
                let port_ok = |p: Option<u32>| p.is_none_or(|p| (1..=4).contains(&p));
                if !port_ok(e.winner_port)
                    || !port_ok(e.quitter_port)
                    || e.players.len() > 4
                    || e.players.iter().any(|p| {
                        !port_ok(Some(p.port)) || p.uid.len() > 64 || p.team_id.is_some_and(|t| t > 8)
                    })
                {
                    return Err((op, "bad ports or players".into()));
                }
                Ok(SpectateRequest::End(e))
            }
            SPECTATE_DIGEST => {
                let slot = slot().ok_or((op, "bad slot".to_string()))?;
                let list = v.get("frms").and_then(|f| f.as_array()).ok_or((op, "no frms".to_string()))?;
                if list.len() > MAX_DIGESTS {
                    return Err((op, "too many digests".into()));
                }
                let mut frms = Vec::with_capacity(list.len());
                for e in list {
                    let pair = e.as_array().filter(|a| a.len() == 2).ok_or((op, "bad digest".to_string()))?;
                    let frame = pair[0].as_u64().and_then(|f| u32::try_from(f).ok());
                    let hash = pair[1].as_str().and_then(decode_hex8);
                    match (frame, hash) {
                        (Some(f), Some(h)) => frms.push((f, h)),
                        _ => return Err((op, "bad digest".into())),
                    }
                }
                Ok(SpectateRequest::Digest { slot, frms })
            }
            SPECTATE_PAUSE => {
                Ok(SpectateRequest::Pause { slot: slot().ok_or((op, "bad slot".to_string()))?, reason: reason()? })
            }
            SPECTATE_RESUME => Ok(SpectateRequest::Resume { slot: slot().ok_or((op, "bad slot".to_string()))? }),
            SPECTATE_STOP => {
                Ok(SpectateRequest::Stop { slot: slot().ok_or((op, "bad slot".to_string()))?, reason: reason()? })
            }
            _ => Err((op, format!("unknown spectate message {kind:?}"))),
        }
    }
}

/// The `op` of a `spectate-*` type for `spectate-error` (`invalid` for one we don't know).
pub fn op_of(kind: &str) -> &'static str {
    match kind {
        SPECTATE_OFFER => SPECTATE_OFFER,
        SPECTATE_END => SPECTATE_END,
        SPECTATE_DIGEST => SPECTATE_DIGEST,
        SPECTATE_PAUSE => SPECTATE_PAUSE,
        SPECTATE_RESUME => SPECTATE_RESUME,
        SPECTATE_STOP => SPECTATE_STOP,
        _ => "invalid",
    }
}

/// A `game_uid`: 32 hex digits.
pub fn decode_uid(s: &str) -> Option<[u8; 16]> {
    if s.len() != 32 {
        return None;
    }
    let v = hex::decode(s).ok()?;
    v.try_into().ok()
}

fn decode_hex8(s: &str) -> Option<[u8; 8]> {
    if s.len() != 16 {
        return None;
    }
    hex::decode(s).ok()?.try_into().ok()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    Primary,
    Witness,
    None,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RoleMsg {
    #[serde(rename = "type")]
    pub kind: String,
    pub game_uid: String,
    pub role: Role,
    pub slot: u32,
    pub from: Option<u32>,
    pub headers: bool,
    pub rate: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SpectateErrorMsg {
    #[serde(rename = "type")]
    pub kind: String,
    pub op: String,
    pub error: String,
}

// ---------------------------------------------------------------- the list (mm → accounts → launcher)

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum GameState {
    #[default]
    Playing,
    Between,
    Over,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct LiveGame {
    pub number: u32,
    /// The game's place in the stream (1-based): a watch from `?game=<place>` starts with it, so
    /// a spectator joining a set mid-way starts at the game being played.
    #[serde(default)]
    pub place: u32,
    pub started_at: String,
    pub stage_id: u32,
    pub state: GameState,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct LivePlayer {
    pub uid: String,
    pub display_name: String,
    pub connect_code: String,
    pub character_id: u32,
    pub costume: u32,
    pub team_id: Option<u32>,
    pub wins: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct LiveEntry {
    pub id: String,
    /// `ranked`, `unranked`, `direct` or `room`.
    pub mode: String,
    /// A room's code.
    pub room: Option<String>,
    pub is_teams: bool,
    pub started_at: String,
    pub game: LiveGame,
    pub players: Vec<LivePlayer>,
    /// Filled in by accounts (mm doesn't know who watches).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spectators: Option<u32>,
}

/// One line of mm's `GET /live/{stream}` (`docs/spectate-protocol.md` §2).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "lowercase")]
pub enum LiveLine {
    /// `game`: the game's place in the stream (1-based); `number`: the game number the players
    /// saw (a desync restart repeats it).
    Game {
        game: u32,
        number: u32,
        uid: String,
    },
    Chunk {
        game: u32,
        i: u32,
        b: String,
    },
    End {
        game: u32,
    },
    Bad {
        game: u32,
        error: String,
    },
    Over,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn frms_raw(first: u32, count: u16, ports: u8, parts: u16) -> Vec<u8> {
        let mut raw = Vec::new();
        raw.extend_from_slice(&first.to_le_bytes());
        raw.extend_from_slice(&count.to_le_bytes());
        raw.push(ports);
        raw.push(0);
        raw.extend(std::iter::repeat_n(7u8, ports.count_ones() as usize * count as usize * PAD_SIZE));
        raw.extend(std::iter::repeat_n(9u8, count as usize * 4));
        raw.extend_from_slice(&parts.to_le_bytes());
        raw.extend(std::iter::repeat_n(1u8, parts as usize * PARTS_SIZE));
        raw
    }

    #[test]
    fn chunks_round_trip_and_damage_is_refused() {
        let raw = frms_raw(120, 60, 0b11, 1);
        let chunk = encode_chunk(b"FRMS", &raw, true);
        let v = parse_chunk(&chunk).unwrap();
        assert_eq!(v.kind, ChunkKind::Frms);
        assert_eq!(v.raw().unwrap(), raw);
        assert_eq!(frames_head(&raw).unwrap(), FramesHead { first_frame: 120, count: 60, ports: 3 });

        let mut bad = chunk.clone();
        *bad.last_mut().unwrap() ^= 1;
        assert!(parse_chunk(&bad).unwrap_err().contains("CRC"));
        assert!(parse_chunk(&chunk[..chunk.len() - 1]).is_err());
        let mut ftrm = chunk.clone();
        ftrm[..4].copy_from_slice(b"FTRM");
        assert!(parse_chunk(&ftrm).unwrap_err().contains("not allowed"));
        let mut flags = chunk.clone();
        flags[4] = 0x80;
        assert!(parse_chunk(&flags).unwrap_err().contains("flags"));
    }

    #[test]
    fn a_lying_raw_length_never_inflates_past_it() {
        let raw = vec![0u8; 50_000];
        let mut chunk = encode_chunk(b"FRMS", &raw, true);
        // Claim 100 bytes: the decoder stops at 101 and the length check refuses it.
        chunk[12..16].copy_from_slice(&100u32.to_le_bytes());
        let v = parse_chunk(&chunk).unwrap();
        assert!(v.raw().unwrap_err().contains("not 100"));
    }

    #[test]
    fn frames_head_checks_the_size() {
        assert!(frames_head(&frms_raw(0, 61, 1, 0)).is_err());
        assert!(frames_head(&frms_raw(0, 10, 0, 0)).is_err());
        let mut raw = frms_raw(0, 10, 0b101, 0);
        assert_eq!(frames_head(&raw).unwrap().count, 10);
        raw.push(0);
        assert!(frames_head(&raw).is_err());
    }

    #[test]
    fn data_packets_split_and_parse() {
        let chunk = encode_chunk(b"SETP", &vec![b'x'; 3000], false);
        let packets = data_packets(5, 2, &chunk);
        assert_eq!(packets.len(), 3);
        let mut joined = Vec::new();
        for p in &packets {
            assert!(p.len() <= MAX_DATA_PACKET);
            let (h, b) = parse_data(p).unwrap();
            assert_eq!((h.slot, h.seq, h.total as usize, h.offset as usize), (5, 2, chunk.len(), joined.len()));
            joined.extend_from_slice(b);
        }
        assert_eq!(joined, chunk);
        let mut over = packets[0].clone();
        over[12..16].copy_from_slice(&10u32.to_le_bytes());
        assert!(parse_data(&over).is_none());
        assert!(parse_data(&packets[0][..DATA_HEADER]).is_none());
    }

    #[test]
    fn requests_parse_with_limits() {
        let uid = "ab".repeat(16);
        let offer = json!({"type": "spectate-offer", "matchId": "m", "gameUid": uid, "game": 1});
        assert!(matches!(SpectateRequest::parse(SPECTATE_OFFER, &offer), Ok(SpectateRequest::Offer(_))));
        let bad = json!({"type": "spectate-offer", "matchId": "m", "gameUid": "zz", "game": 1});
        assert_eq!(SpectateRequest::parse(SPECTATE_OFFER, &bad).unwrap_err().0, SPECTATE_OFFER);
        let digest = json!({"slot": 3, "frms": [[60, "00112233445566ff"]]});
        assert_eq!(
            SpectateRequest::parse(SPECTATE_DIGEST, &digest).unwrap(),
            SpectateRequest::Digest { slot: 3, frms: vec![(60, [0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xff])] }
        );
        let many: Vec<_> = (0..31).map(|i| json!([i, "0000000000000000"])).collect();
        assert!(SpectateRequest::parse(SPECTATE_DIGEST, &json!({"slot": 3, "frms": many})).is_err());
        assert!(SpectateRequest::parse(SPECTATE_PAUSE, &json!({"slot": 1, "reason": "lol"})).is_err());
        assert_eq!(
            SpectateRequest::parse(SPECTATE_PAUSE, &json!({"slot": 1, "reason": "rtt"})).unwrap(),
            SpectateRequest::Pause { slot: 1, reason: "rtt".into() }
        );
        assert_eq!(SpectateRequest::parse("spectate-nope", &json!({})).unwrap_err().0, "invalid");
        let end = json!({"matchId": "m", "gameUid": uid, "endMethod": "game", "winnerPort": 2,
                         "players": [{"port": 1, "uid": "a"}, {"port": 2, "uid": "b", "teamId": null}]});
        let Ok(SpectateRequest::End(e)) = SpectateRequest::parse(SPECTATE_END, &end) else { panic!("{end}") };
        assert_eq!((e.winner_port, e.quitter_port, e.players.len()), (Some(2), None, 2));
        for bad in [
            json!({"matchId": "m", "gameUid": uid, "endMethod": "won"}),
            json!({"matchId": "m", "gameUid": uid, "endMethod": "game", "winnerPort": 5}),
            json!({"matchId": "m", "gameUid": uid, "endMethod": "game", "players": [{"port": 0, "uid": "a"}]}),
        ] {
            assert_eq!(SpectateRequest::parse(SPECTATE_END, &bad).unwrap_err().0, SPECTATE_END, "{bad}");
        }
    }
}
