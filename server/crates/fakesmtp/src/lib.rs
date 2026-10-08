//! A tiny SMTP server for tests: it listens on `127.0.0.1:<ephemeral>`, speaks
//! just enough ESMTP for a client like lettre (EHLO, AUTH PLAIN/LOGIN, MAIL,
//! RCPT, DATA, RSET, NOOP, QUIT), never relays anything, and records what it
//! saw. It also counts every TCP connection, so a test can point any network
//! setting (an SMTP host, an HTTP API URL) at it and assert that nothing ever
//! connected.
//!
//! No TLS: clients must use plain SMTP (`SMTP_TLS=none`). A client that
//! insists on STARTTLS gets an EHLO answer without it, and must give up.

use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use base64::Engine;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};

/// How the server answers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Accept any credentials and every message.
    Accept,
    /// Answer every AUTH with `535 5.7.8`.
    RejectAuth,
    /// Accept the TCP connection and never say anything (for timeouts).
    Silent,
}

/// One client connection, as the server saw it.
#[derive(Debug, Clone, Default)]
pub struct Session {
    pub ehlo: Option<String>,
    /// Decoded (username, password) from AUTH.
    pub auth: Option<(String, String)>,
    pub mail_from: Option<String>,
    pub rcpt_to: Vec<String>,
    /// The message (headers and body), dot-unstuffed, CRLF line ends.
    pub data: Option<String>,
    pub quit: bool,
}

pub struct FakeSmtp {
    pub addr: SocketAddr,
    connections: Arc<AtomicUsize>,
    sessions: Arc<Mutex<Vec<Session>>>,
    task: tokio::task::JoinHandle<()>,
}

impl Drop for FakeSmtp {
    fn drop(&mut self) {
        self.task.abort();
    }
}

impl FakeSmtp {
    pub async fn start(mode: Mode) -> std::io::Result<FakeSmtp> {
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let connections = Arc::new(AtomicUsize::new(0));
        let sessions: Arc<Mutex<Vec<Session>>> = Default::default();
        let (c, s) = (connections.clone(), sessions.clone());
        let task = tokio::spawn(async move {
            while let Ok((stream, _)) = listener.accept().await {
                c.fetch_add(1, Ordering::SeqCst);
                let idx = {
                    let mut all = s.lock().unwrap();
                    all.push(Session::default());
                    all.len() - 1
                };
                let s = s.clone();
                tokio::spawn(async move {
                    let _ = serve(stream, mode, s, idx).await;
                });
            }
        });
        Ok(FakeSmtp { addr, connections, sessions, task })
    }

    pub fn port(&self) -> u16 {
        self.addr.port()
    }

    /// TCP connections accepted so far (any protocol).
    pub fn connections(&self) -> usize {
        self.connections.load(Ordering::SeqCst)
    }

    pub fn sessions(&self) -> Vec<Session> {
        self.sessions.lock().unwrap().clone()
    }

    /// Sessions that delivered a message.
    pub fn messages(&self) -> Vec<Session> {
        self.sessions().into_iter().filter(|s| s.data.is_some()).collect()
    }

    /// Waits up to `timeout` for `n` delivered messages.
    pub async fn wait_for_messages(&self, n: usize, timeout: Duration) -> Vec<Session> {
        let end = tokio::time::Instant::now() + timeout;
        loop {
            let m = self.messages();
            if m.len() >= n || tokio::time::Instant::now() >= end {
                return m;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }
}

fn b64(s: &str) -> String {
    base64::engine::general_purpose::STANDARD
        .decode(s.trim())
        .map(|b| String::from_utf8_lossy(&b).into_owned())
        .unwrap_or_default()
}

/// `<addr> SIZE=…` → `addr`.
fn angle(arg: &str) -> String {
    let arg = arg.trim();
    match (arg.find('<'), arg.find('>')) {
        (Some(a), Some(b)) if b > a => arg[a + 1..b].to_string(),
        _ => arg.split_whitespace().next().unwrap_or("").to_string(),
    }
}

async fn serve(stream: TcpStream, mode: Mode, sessions: Arc<Mutex<Vec<Session>>>, idx: usize) -> std::io::Result<()> {
    let (rd, mut wr) = stream.into_split();
    let mut rd = BufReader::new(rd);
    if mode == Mode::Silent {
        let mut buf = [0u8; 1024];
        while rd.read(&mut buf).await? > 0 {}
        return Ok(());
    }
    let update = |f: &dyn Fn(&mut Session)| f(&mut sessions.lock().unwrap()[idx]);
    wr.write_all(b"220 fake.smtp.test ESMTP fakesmtp\r\n").await?;
    let mut line = String::new();
    async fn next(rd: &mut BufReader<tokio::net::tcp::OwnedReadHalf>, line: &mut String) -> std::io::Result<bool> {
        line.clear();
        Ok(rd.read_line(line).await? > 0)
    }
    while next(&mut rd, &mut line).await? {
        let cmd = line.trim_end_matches(['\r', '\n']).to_string();
        let (verb, arg) = match cmd.split_once(' ') {
            Some((v, a)) => (v.to_ascii_uppercase(), a.to_string()),
            None => (cmd.to_ascii_uppercase(), String::new()),
        };
        match verb.as_str() {
            "EHLO" => {
                update(&|s| s.ehlo = Some(arg.clone()));
                wr.write_all(b"250-fake.smtp.test\r\n250-AUTH PLAIN LOGIN\r\n250 8BITMIME\r\n").await?;
            }
            "HELO" => {
                update(&|s| s.ehlo = Some(arg.clone()));
                wr.write_all(b"250 fake.smtp.test\r\n").await?;
            }
            "AUTH" => {
                let (mech, initial) = match arg.split_once(' ') {
                    Some((m, i)) => (m.to_ascii_uppercase(), Some(i.to_string())),
                    None => (arg.to_ascii_uppercase(), None),
                };
                let creds = match mech.as_str() {
                    "PLAIN" => {
                        let payload = match initial {
                            Some(i) => i,
                            None => {
                                wr.write_all(b"334 \r\n").await?;
                                if !next(&mut rd, &mut line).await? {
                                    break;
                                }
                                line.clone()
                            }
                        };
                        let decoded = b64(&payload);
                        let mut parts = decoded.split('\0').skip(1);
                        Some((parts.next().unwrap_or("").to_string(), parts.next().unwrap_or("").to_string()))
                    }
                    "LOGIN" => {
                        let user = match initial {
                            Some(i) => b64(&i),
                            None => {
                                wr.write_all(b"334 VXNlcm5hbWU6\r\n").await?;
                                if !next(&mut rd, &mut line).await? {
                                    break;
                                }
                                b64(&line)
                            }
                        };
                        wr.write_all(b"334 UGFzc3dvcmQ6\r\n").await?;
                        if !next(&mut rd, &mut line).await? {
                            break;
                        }
                        Some((user, b64(&line)))
                    }
                    _ => None,
                };
                match creds {
                    None => wr.write_all(b"504 5.5.4 Unrecognized authentication type\r\n").await?,
                    Some(c) => {
                        update(&|s| s.auth = Some(c.clone()));
                        if mode == Mode::RejectAuth {
                            wr.write_all(b"535 5.7.8 Authentication credentials invalid\r\n").await?;
                        } else {
                            wr.write_all(b"235 2.7.0 Authentication successful\r\n").await?;
                        }
                    }
                }
            }
            "MAIL" => {
                let from = angle(arg.split_once(':').map(|x| x.1).unwrap_or(""));
                update(&|s| s.mail_from = Some(from.clone()));
                wr.write_all(b"250 2.1.0 Ok\r\n").await?;
            }
            "RCPT" => {
                let to = angle(arg.split_once(':').map(|x| x.1).unwrap_or(""));
                update(&|s| s.rcpt_to.push(to.clone()));
                wr.write_all(b"250 2.1.5 Ok\r\n").await?;
            }
            "DATA" => {
                wr.write_all(b"354 End data with <CR><LF>.<CR><LF>\r\n").await?;
                let mut data = String::new();
                loop {
                    if !next(&mut rd, &mut line).await? {
                        return Ok(());
                    }
                    if line == ".\r\n" || line == ".\n" {
                        break;
                    }
                    data.push_str(line.strip_prefix('.').unwrap_or(&line));
                }
                update(&|s| s.data = Some(data.clone()));
                wr.write_all(b"250 2.0.0 Ok: queued as FAKE\r\n").await?;
            }
            "RSET" => {
                update(&|s| {
                    s.mail_from = None;
                    s.rcpt_to.clear();
                });
                wr.write_all(b"250 2.0.0 Ok\r\n").await?;
            }
            "NOOP" => wr.write_all(b"250 2.0.0 Ok\r\n").await?,
            "QUIT" => {
                update(&|s| s.quit = true);
                wr.write_all(b"221 2.0.0 Bye\r\n").await?;
                break;
            }
            _ => wr.write_all(b"502 5.5.2 Command not recognized\r\n").await?,
        }
    }
    Ok(())
}
