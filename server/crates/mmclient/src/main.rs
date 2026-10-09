//! `mmclient`: run a matchmaking search like the game would and print the
//! result as one JSON object on stdout. Exit code: 0 matched, 2 create-ticket
//! error, 3 get-ticket error, 4 client timeout, 5 P2P connect failed, 1 other.
//!
//! ```text
//! mmclient search --server 127.0.0.1:43113 --user-json path/to/user.json --code ABCD#123 --punch
//! mmclient raw --server 127.0.0.1:43113 --data '{"type":"bogus"}'
//! mmclient online --user-json a.json --hold-secs 60
//! mmclient room --user-json a.json --create --open 3,4 --teams --ready --play --hold-secs 600
//! mmclient room --user-json b.json --join KFQB --team 1 --ready --play --back-after-secs 5
//! ```
//!
//! `online` and `room` keep an online connection open like a running game and print every
//! message from mm as one JSON line (plus `{"type":"mmclient-ticket",...}` with the result of a
//! room game ticket). `room` exits 0 when its time is up or it leaves the room, 2 if `hello` is
//! refused, 3 if the create or join is refused, 1 on other errors.

use std::net::Ipv4Addr;
use std::time::Duration;

use std::io::Write;
use std::time::Instant;

use clap::{Args, Parser, Subcommand, ValueEnum};
use common::rooms::RoomRequest;
use mmclient::room::{OnlineClient, Received};
use mmclient::{CodeEncoding, Credentials, SearchOptions, UserJsonFile};
use serde_json::{json, Value};

#[derive(Parser)]
#[command(name = "mmclient", version, about = "Matchmaking ticket client (Slippi protocol)")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Clone, Copy, ValueEnum)]
enum ModeArg {
    Ranked,
    Unranked,
    Direct,
    Teams,
    Party,
}

#[derive(Subcommand)]
enum Cmd {
    /// Create a ticket and wait for a match.
    Search {
        /// mm server host[:port] (default port 43113).
        #[arg(long, env = "MM_SERVER", default_value = "127.0.0.1:43113")]
        server: String,
        /// Read uid/playKey/connectCode/displayName from a user.json.
        #[arg(long, conflicts_with_all = ["uid", "play_key"])]
        user_json: Option<std::path::PathBuf>,
        #[arg(long, requires = "play_key")]
        uid: Option<String>,
        #[arg(long)]
        play_key: Option<String>,
        /// Target connect code (Direct/Teams).
        #[arg(long, default_value = "")]
        code: String,
        #[arg(long, value_enum, default_value = "direct")]
        mode: ModeArg,
        /// Raw mode number instead of --mode (to test unknown modes).
        #[arg(long)]
        mode_number: Option<u8>,
        #[arg(long, value_enum, default_value = "fullwidth")]
        encoding: EncodingArg,
        #[arg(long, default_value = "0.1.0")]
        app_version: String,
        /// Force the local UDP port (Slippi "Force Netplay Port").
        #[arg(long)]
        port: Option<u16>,
        /// Force the reported LAN IP (Slippi "Force LAN IP").
        #[arg(long)]
        lan_ip: Option<Ipv4Addr>,
        /// Stop waiting for a match after this many seconds (0 = wait forever, like the game).
        #[arg(long, default_value_t = 0)]
        timeout_secs: u64,
        /// After matching, open the P2P connection from the same port (8 s window).
        #[arg(long)]
        punch: bool,
        /// With --punch: after a failed P2P connect, search again with a new ticket up to N times
        /// (Slippi's 1v1 behaviour).
        #[arg(long, default_value_t = 0)]
        requeue: u32,
    },
    /// Keep an online connection open (`hello`), as a running, logged-in game does.
    Online {
        #[command(flatten)]
        who: Who,
        /// Stay this long, then disconnect.
        #[arg(long, default_value_t = 60)]
        hold_secs: u64,
    },
    /// Create or join a room and stay in it, printing every message.
    Room(Box<RoomArgs>),
    /// Send one raw packet and print the replies.
    Raw {
        #[arg(long, env = "MM_SERVER", default_value = "127.0.0.1:43113")]
        server: String,
        #[arg(long)]
        data: String,
        #[arg(long, default_value_t = 3)]
        wait_secs: u64,
    },
}

/// The server and the account.
#[derive(Args, Clone)]
struct Who {
    /// mm server host[:port] (default port 43113).
    #[arg(long, env = "MM_SERVER", default_value = "127.0.0.1:43113")]
    server: String,
    /// Read uid/playKey from a user.json.
    #[arg(long, conflicts_with_all = ["uid", "play_key"])]
    user_json: Option<std::path::PathBuf>,
    #[arg(long, requires = "play_key")]
    uid: Option<String>,
    #[arg(long)]
    play_key: Option<String>,
    #[arg(long, default_value = "0.1.0")]
    app_version: String,
}

impl Who {
    fn creds(&self) -> anyhow::Result<Credentials> {
        load_creds(self.user_json.clone(), self.uid.clone(), self.play_key.clone())
    }
}

#[derive(Args)]
struct RoomArgs {
    #[command(flatten)]
    who: Who,
    /// Create a room (as its host).
    #[arg(long, conflicts_with = "join")]
    create: bool,
    /// Join the room with this code.
    #[arg(long)]
    join: Option<String>,
    /// Host: make the room private (public by default).
    #[arg(long)]
    private: bool,
    /// Host: open these slots after creating (e.g. `3,4`).
    #[arg(long, value_delimiter = ',')]
    open: Vec<u8>,
    /// Host: close these slots after creating.
    #[arg(long, value_delimiter = ',')]
    close: Vec<u8>,
    /// Host: turn Teams on.
    #[arg(long)]
    teams: bool,
    /// Pick a team colour (0 red, 1 blue, 2 green).
    #[arg(long)]
    team: Option<u8>,
    /// Press START (ready) right away.
    #[arg(long)]
    ready: bool,
    /// The lock-in sent with `--ready`.
    #[arg(long)]
    character: Option<u8>,
    #[arg(long)]
    costume: Option<u8>,
    /// When the room starts, send the room game ticket (from a new port, as the game does).
    #[arg(long)]
    play: bool,
    /// After the game was matched, wait this long and send `room-back` (no `room-back` without it:
    /// the room stays "in game").
    #[arg(long)]
    back_after_secs: Option<u64>,
    /// Exit (drop the connection, like a game that closes) as soon as the game was matched.
    #[arg(long)]
    quit_after_match: bool,
    /// Stay this long in total, then leave the room and disconnect.
    #[arg(long, default_value_t = 600)]
    hold_secs: u64,
}

fn load_creds(
    user_json: Option<std::path::PathBuf>,
    uid: Option<String>,
    play_key: Option<String>,
) -> anyhow::Result<Credentials> {
    Ok(match (user_json, uid, play_key) {
        (Some(path), _, _) => serde_json::from_str::<UserJsonFile>(&std::fs::read_to_string(path)?)?.into(),
        (None, Some(uid), Some(play_key)) => Credentials { uid, play_key, connect_code: None, display_name: None },
        _ => anyhow::bail!("give --user-json or --uid and --play-key"),
    })
}

fn print_line(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn run_online(who: Who, hold_secs: u64) -> anyhow::Result<i32> {
    let server = mmclient::resolve(&who.server)?;
    let (mut client, hello) = OnlineClient::connect(server, &who.creds()?, &who.app_version)?;
    print_line(&hello);
    if hello.get("error").is_some() {
        return Ok(2);
    }
    let deadline = Instant::now() + Duration::from_secs(hold_secs);
    while Instant::now() < deadline {
        match client.recv(Duration::from_millis(200)) {
            Received::Message(m) => print_line(&m),
            Received::Disconnected => {
                print_line(&json!({"type": "mmclient-disconnected"}));
                return Ok(1);
            }
            Received::Timeout => {}
        }
    }
    client.close();
    Ok(0)
}

fn run_room(a: RoomArgs) -> anyhow::Result<i32> {
    anyhow::ensure!(a.create || a.join.is_some(), "give --create or --join CODE");
    let server = mmclient::resolve(&a.who.server)?;
    let creds = a.who.creds()?;
    let (mut client, hello) = OnlineClient::connect(server, &creds, &a.who.app_version)?;
    print_line(&hello);
    if hello.get("error").is_some() {
        return Ok(2);
    }
    client.send(&match &a.join {
        Some(code) => RoomRequest::Join { code: code.clone() },
        None => RoomRequest::Create { public: !a.private },
    });
    let first = client.wait_for(Duration::from_secs(10), |m| m["type"] == "room-state" || m["type"] == "room-error")?;
    print_line(&first);
    if first["type"] == "room-error" {
        client.close();
        return Ok(3);
    }
    let code = first["code"].as_str().unwrap_or_default().to_string();
    if a.create {
        for &s in &a.open {
            client.send(&RoomRequest::Slot { slot: s, open: true });
        }
        for &s in &a.close {
            client.send(&RoomRequest::Slot { slot: s, open: false });
        }
        if a.teams {
            client.send(&RoomRequest::Teams { on: true });
        }
    }
    if let Some(team) = a.team {
        client.send(&RoomRequest::Team { team });
    }
    if a.ready {
        client.send(&RoomRequest::Ready { ready: true, character: a.character, costume: a.costume });
    }
    let deadline = Instant::now() + Duration::from_secs(a.hold_secs);
    let mut ticket: Option<std::thread::JoinHandle<anyhow::Result<mmclient::SearchResult>>> = None;
    let mut back_at: Option<Instant> = None;
    while Instant::now() < deadline {
        match client.recv(Duration::from_millis(100)) {
            Received::Message(m) => {
                print_line(&m);
                if m["type"] == "room-left" {
                    client.close();
                    return Ok(0);
                }
                if m["type"] == "room-start" && a.play && ticket.is_none() {
                    let (creds, code) = (creds.clone(), code.clone());
                    ticket = Some(std::thread::spawn(move || mmclient::room::room_ticket(server, creds, &code)));
                }
            }
            Received::Disconnected => {
                print_line(&json!({"type": "mmclient-disconnected"}));
                return Ok(1);
            }
            Received::Timeout => {}
        }
        if ticket.as_ref().is_some_and(|t| t.is_finished()) {
            let result = ticket.take().expect("checked").join().map_err(|_| anyhow::anyhow!("ticket thread"))??;
            let matched = result.status == mmclient::Status::Matched;
            print_line(&json!({"type": "mmclient-ticket", "result": result}));
            if matched && a.quit_after_match {
                // Like a game that closes in the middle of the match: no leave, no goodbye.
                return Ok(0);
            }
            if matched {
                back_at = a.back_after_secs.map(|s| Instant::now() + Duration::from_secs(s));
            }
        }
        if back_at.is_some_and(|t| Instant::now() >= t) {
            back_at = None;
            client.send(&RoomRequest::Back);
            if a.ready {
                client.send(&RoomRequest::Ready { ready: true, character: a.character, costume: a.costume });
            }
        }
    }
    client.send(&RoomRequest::Leave);
    let _ = client.drain(Duration::from_millis(300));
    client.close();
    Ok(0)
}

#[derive(Clone, Copy, ValueEnum)]
enum EncodingArg {
    Fullwidth,
    Ascii,
}

fn main() {
    let code = match run() {
        Ok(c) => c,
        Err(e) => {
            println!("{}", serde_json::json!({"status": "client-error", "error": format!("{e:#}")}));
            1
        }
    };
    std::process::exit(code);
}

fn run() -> anyhow::Result<i32> {
    match Cli::parse().cmd {
        Cmd::Search {
            server,
            user_json,
            uid,
            play_key,
            code,
            mode,
            mode_number,
            encoding,
            app_version,
            port,
            lan_ip,
            timeout_secs,
            punch,
            requeue,
        } => {
            let creds = load_creds(user_json, uid, play_key)?;
            let mode = mode_number.unwrap_or(match mode {
                ModeArg::Ranked => 0,
                ModeArg::Unranked => 1,
                ModeArg::Direct => 2,
                ModeArg::Teams => 3,
                ModeArg::Party => 4,
            });
            let opts = SearchOptions {
                server: mmclient::resolve(&server)?,
                creds,
                mode,
                target: code,
                encoding: match encoding {
                    EncodingArg::Fullwidth => CodeEncoding::Fullwidth,
                    EncodingArg::Ascii => CodeEncoding::Ascii,
                },
                app_version,
                local_port: port,
                lan_ip,
                match_timeout: (timeout_secs > 0).then(|| Duration::from_secs(timeout_secs)),
                punch,
                punch_timeout: Duration::from_secs(8),
                requeue,
            };
            let result = mmclient::search(&opts)?;
            println!("{}", serde_json::to_string(&result)?);
            Ok(result.status.exit_code())
        }
        Cmd::Online { who, hold_secs } => run_online(who, hold_secs),
        Cmd::Room(a) => run_room(*a),
        Cmd::Raw { server, data, wait_secs } => {
            let (replies, disconnected) = mmclient::send_raw_packet(
                mmclient::resolve(&server)?,
                data.as_bytes(),
                Duration::from_secs(wait_secs),
            )?;
            println!("{}", serde_json::json!({"replies": replies, "serverDisconnected": disconnected}));
            Ok(0)
        }
    }
}
