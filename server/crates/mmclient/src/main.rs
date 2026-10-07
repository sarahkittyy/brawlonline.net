//! `mmclient`: run a matchmaking search like the game would and print the
//! result as one JSON object on stdout. Exit code: 0 matched, 2 create-ticket
//! error, 3 get-ticket error, 4 client timeout, 5 P2P connect failed, 1 other.
//!
//! ```text
//! mmclient search --server 127.0.0.1:43113 --user-json path/to/user.json --code ABCD#123 --punch
//! mmclient raw --server 127.0.0.1:43113 --data '{"type":"bogus"}'
//! ```

use std::net::Ipv4Addr;
use std::time::Duration;

use clap::{Parser, Subcommand, ValueEnum};
use mmclient::{CodeEncoding, Credentials, SearchOptions, UserJsonFile};

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
            let creds: Credentials = match (user_json, uid, play_key) {
                (Some(path), _, _) => serde_json::from_str::<UserJsonFile>(&std::fs::read_to_string(path)?)?.into(),
                (None, Some(uid), Some(play_key)) => {
                    Credentials { uid, play_key, connect_code: None, display_name: None }
                }
                _ => anyhow::bail!("give --user-json or --uid and --play-key"),
            };
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
