//! Staging only: made-up live state, so a launcher pointed at a staging server with nobody else on
//! it still shows full screens (docs/staging.md, "Fake players"). Off unless the `STAGING_FAKE_STATE`
//! environment variable names a JSON file; `tools/staging/staging.py` sets it for the staging
//! server and production never does.
//!
//! `tools/staging/seed_fake.py` writes the file beside the fake accounts it seeds: public rooms,
//! friends-only rooms, Brawl Mods rooms, live games, the queue counts and the players online, all
//! with the fake accounts' real uids, so everything the database knows about them (names, flags,
//! mains, ranks, ratings) comes through as for anyone. The file is merged into what accounts
//! reads from mm ([`status`]), into the social stream's online list ([`members`]) and into the
//! live games' spectator counts ([`spectators`]). It is read again when it changes.
//!
//! Nobody is behind a fake: joining a fake room fails in the game (mm has no such room), watching
//! a fake game ends at once ("This game has ended"), and a DM or invite to a fake player answers
//! "That player is offline".

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime};

use chrono::{DateTime, Utc};
use common::mods::PublicModRoom;
use common::rooms::{MmStatus, ModRoomMember, PublicRoom, QueueCounts, RoomMember, RoomStatus};
use common::spectate::{GameState, LiveEntry};
use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::social::Member;

/// The environment variable naming the file.
pub const ENV: &str = "STAGING_FAKE_STATE";

/// How long a fake game's clock runs before it starts over (the next game of the set).
const GAME_SECS: u64 = 420;

/// The file (`seed_fake.py`). Counts are added to mm's; lists are added after mm's entries.
#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct File {
    online: usize,
    queues: QueueCounts,
    playing: QueueCounts,
    rooms: Vec<PublicRoom>,
    friend_rooms: Vec<PublicRoom>,
    mod_rooms: Vec<PublicModRoom>,
    members: Vec<RoomMember>,
    mod_members: Vec<ModRoomMember>,
    live: Vec<FakeLive>,
    /// The social stream's online list.
    players: Vec<FakePlayer>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FakeLive {
    #[serde(flatten)]
    entry: LiveEntry,
    /// Seconds into the game when the file was read; the clock runs on from there.
    #[serde(default)]
    clock: u64,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FakePlayer {
    uid: String,
    display_name: String,
    connect_code: String,
    #[serde(flatten)]
    tag: crate::profile::NameTag,
}

struct Loaded {
    path: PathBuf,
    modified: Option<SystemTime>,
    at: Instant,
    file: Arc<File>,
    members: Arc<Vec<Member>>,
}

static STATE: Mutex<Option<Loaded>> = Mutex::new(None);

/// The file, read again when it changed; `None` when off or unreadable.
fn current() -> Option<(Arc<File>, Arc<Vec<Member>>, Instant)> {
    let path = PathBuf::from(std::env::var_os(ENV).filter(|p| !p.is_empty())?);
    let modified = std::fs::metadata(&path).ok().map(|m| m.modified().ok());
    let mut state = STATE.lock().unwrap();
    let Some(modified) = modified else {
        if state.take().is_some() {
            tracing::info!(path = %path.display(), "staging fake state gone: fakes off");
        }
        return None;
    };
    if let Some(s) = state.as_ref().filter(|s| s.path == path && s.modified == modified) {
        return Some((s.file.clone(), s.members.clone(), s.at));
    }
    let file: File = match std::fs::read(&path)
        .map_err(|e| e.to_string())
        .and_then(|b| serde_json::from_slice(&b).map_err(|e| e.to_string()))
    {
        Ok(f) => f,
        Err(e) => {
            tracing::warn!(path = %path.display(), "staging fake state unreadable: {e}");
            *state = None;
            return None;
        }
    };
    tracing::info!(
        path = %path.display(),
        rooms = file.rooms.len() + file.friend_rooms.len(),
        mod_rooms = file.mod_rooms.len(),
        live = file.live.len(),
        players = file.players.len(),
        "staging fake state loaded"
    );
    let members = Arc::new(file.players.iter().map(member).collect());
    let loaded = Loaded { path, modified, at: Instant::now(), file: Arc::new(file), members };
    let out = (loaded.file.clone(), loaded.members.clone(), loaded.at);
    *state = Some(loaded);
    Some(out)
}

/// A fake player as the online list has them, with chat keys made from the uid (valid, so the
/// launcher takes them; nobody holds the secrets that answer).
fn member(p: &FakePlayer) -> Member {
    let secret = |label: &str| -> [u8; 32] { Sha256::digest(format!("staging-fake-{label}:{}", p.uid)).into() };
    let id_seed = secret("id");
    let kx = common::chat::kx_public(&secret("kx"));
    let sig = common::chat::sign_kx(&id_seed, &crate::social::session_group(&p.uid), &kx);
    Member {
        uid: p.uid.clone(),
        display_name: p.display_name.clone(),
        connect_code: p.connect_code.clone(),
        id_key: hex::encode(common::chat::identity_public(&id_seed)),
        kx: hex::encode(kx),
        kx_sig: hex::encode(sig),
        tag: p.tag.clone(),
    }
}

/// mm's order of public rooms: joinable first, then waiting before in game, then fuller.
fn room_order(a: &PublicRoom, b: &PublicRoom) -> std::cmp::Ordering {
    b.joinable
        .cmp(&a.joinable)
        .then((a.status != RoomStatus::Waiting).cmp(&(b.status != RoomStatus::Waiting)))
        .then(b.players.cmp(&a.players))
}

fn rfc3339(t: DateTime<Utc>) -> String {
    t.to_rfc3339_opts(chrono::SecondsFormat::Millis, true)
}

/// mm's snapshot with the fake rooms, games and counts added (unchanged when off). Fakes whose
/// code, stream or uid mm already has are left out.
pub fn status(mut s: MmStatus) -> MmStatus {
    let Some((f, _, at)) = current() else { return s };
    s.online += f.online;
    for (to, add) in [(&mut s.queues, &f.queues), (&mut s.playing, &f.playing)] {
        to.unranked += add.unranked;
        to.ranked += add.ranked;
        to.twos += add.twos;
    }
    for (to, add) in [(&mut s.rooms, &f.rooms), (&mut s.friend_rooms, &f.friend_rooms)] {
        let fresh: Vec<PublicRoom> = add.iter().filter(|r| !to.iter().any(|x| x.code == r.code)).cloned().collect();
        to.extend(fresh);
        to.sort_by(room_order);
    }
    let fresh: Vec<PublicModRoom> =
        f.mod_rooms.iter().filter(|r| !s.mod_room_index.iter().any(|x| x.code == r.code)).cloned().collect();
    s.mod_rooms.extend(fresh.iter().cloned());
    s.mod_rooms.sort_by(|a, b| b.players.cmp(&a.players));
    s.mod_room_index.extend(fresh);
    let fresh: Vec<RoomMember> =
        f.members.iter().filter(|m| !s.members.iter().any(|x| x.uid == m.uid)).cloned().collect();
    s.members.extend(fresh);
    let fresh: Vec<ModRoomMember> =
        f.mod_members.iter().filter(|m| !s.mod_members.iter().any(|x| x.uid == m.uid)).cloned().collect();
    s.mod_members.extend(fresh);
    let now = Utc::now();
    let ran = at.elapsed().as_secs();
    let real: Vec<String> = s.live.iter().map(|x| x.id.clone()).collect();
    for l in f.live.iter().filter(|l| !real.contains(&l.entry.id)) {
        let mut e = l.entry.clone();
        e.spectators = None;
        if e.game.state == GameState::Playing {
            let into = (ran + l.clock) % GAME_SECS;
            let game_start = now - chrono::Duration::seconds(into as i64);
            e.game.started_at = rfc3339(game_start);
            let earlier = i64::from(e.game.number.max(1) - 1) * GAME_SECS as i64 + 60;
            e.started_at = rfc3339(game_start - chrono::Duration::seconds(earlier));
        }
        s.live.push(e);
    }
    s
}

/// The fake players online, for the social stream's `welcome` (empty when off).
pub fn members() -> Arc<Vec<Member>> {
    current().map(|(_, m, _)| m).unwrap_or_default()
}

/// Adds the fake games' spectators to the counts `GET /v1/live` filled in.
pub fn spectators(games: &mut [LiveEntry]) {
    let Some((f, _, _)) = current() else { return };
    for g in games.iter_mut() {
        if let Some(l) = f.live.iter().find(|l| l.entry.id == g.id) {
            g.spectators = Some(g.spectators.unwrap_or(0) + l.entry.spectators.unwrap_or(0));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fake_players_keys_verify() {
        let p = FakePlayer {
            uid: "0f8fad5b-d9cb-469f-a165-70867728950e".into(),
            display_name: "Peachy".into(),
            connect_code: "PEAC#33".into(),
            tag: Default::default(),
        };
        let m = member(&p);
        let id: [u8; 32] = hex::decode(&m.id_key).unwrap().try_into().unwrap();
        let kx: [u8; 32] = hex::decode(&m.kx).unwrap().try_into().unwrap();
        let sig: [u8; 64] = hex::decode(&m.kx_sig).unwrap().try_into().unwrap();
        assert!(common::chat::parse_identity_key(&m.id_key).is_some());
        assert!(common::chat::verify_kx(&id, &crate::social::session_group(&p.uid), &kx, &sig));
        assert_eq!(member(&p), m, "the same keys every time");
    }

    #[test]
    fn the_file_format_reads() {
        let f: File = serde_json::from_value(serde_json::json!({
            "online": 3,
            "queues": {"unranked": 1, "ranked": 2},
            "rooms": [{"code": "KFQB", "host": "Peachy", "players": 1, "openSlots": 4, "mode": "1v1",
                       "status": "waiting", "names": ["Peachy"], "uids": ["0f8fad5b-d9cb-469f-a165-70867728950e"],
                       "joinable": true,
                       "ruleset": {"code": null, "name": "P+ default", "version": 0, "by": "", "basedOn": null,
                                   "bestOf": 0, "stageChoice": "loser-picks"}}],
            "live": [{"id": "f0000000000a", "mode": "ranked", "room": null, "isTeams": false,
                      "startedAt": "2026-11-14T20:00:00.000Z",
                      "game": {"number": 2, "place": 2, "startedAt": "2026-11-14T20:04:00.000Z", "stageId": 1,
                               "state": "playing"},
                      "players": [], "spectators": 6, "clock": 100}],
            "players": [{"uid": "0f8fad5b-d9cb-469f-a165-70867728950e", "displayName": "Peachy",
                         "connectCode": "PEAC#33", "country": "jp", "main": "peach"}],
        }))
        .unwrap();
        assert_eq!(f.rooms.len(), 1);
        assert_eq!((f.live[0].clock, f.live[0].entry.spectators), (100, Some(6)));
        assert_eq!(f.players[0].tag.main.as_deref(), Some("peach"));
    }
}
