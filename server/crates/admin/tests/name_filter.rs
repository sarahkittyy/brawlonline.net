//! `admin filter`: the name filter's rules in the database. A draft rule changes nothing until it
//! is on; `filter test` shows what it would change over the players' names and the samples, and
//! asks the model only about texts its prompt has not seen. Runs the real `admin` executable
//! against a fresh database and a stand-in for DeepInfra.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use accounts::slur_filter::{self as sf, SlurFilter, What};
use accounts::store;

/// A stand-in for DeepInfra: YES for texts with "bad" in them; counts the requests.
async fn fake_deepinfra() -> (String, Arc<AtomicUsize>) {
    use axum::{extract::State, routing::post, Json, Router};
    let asked = Arc::new(AtomicUsize::new(0));
    let app = Router::new()
        .route(
            "/chat",
            post(|State(asked): State<Arc<AtomicUsize>>, Json(body): Json<serde_json::Value>| async move {
                asked.fetch_add(1, Ordering::SeqCst);
                let text = body["messages"][1]["content"].as_str().unwrap_or_default().to_lowercase();
                let word = if text.contains("bad") { "YES" } else { "NO" };
                Json(serde_json::json!({"choices": [{"message": {"role": "assistant", "content": word}}]}))
            }),
        )
        .with_state(asked.clone());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    (format!("http://{addr}/chat"), asked)
}

async fn run(url: &str, chat: &str, args: &[&str]) -> (bool, String) {
    let out = tokio::process::Command::new(env!("CARGO_BIN_EXE_admin"))
        .args(args)
        .current_dir(std::env::temp_dir())
        .env("DATABASE_URL", url)
        .env("DEEPINFRA_API_KEY", "di_test")
        .env("SLUR_FILTER_URL", chat)
        .env("EDITOR", "false-editor-never-run")
        .output()
        .await
        .unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    (out.status.success(), text)
}

/// The lines of one `filter test` section.
fn section(text: &str, title: &str) -> Vec<String> {
    let mut lines = text.lines().skip_while(|l| !l.starts_with(&format!("{title} (")));
    let Some(head) = lines.next() else { panic!("no section {title:?} in:\n{text}") };
    let n: usize = head[title.len() + 2..].split(')').next().unwrap().parse().unwrap();
    lines.take(n).map(|l| l.trim().to_string()).collect()
}

#[tokio::test]
async fn rules_are_drafted_tested_and_turned_on() {
    let (pool, name) = e2e::fresh_db().await.expect("postgres (see server/README.md)");
    let url = e2e::db_url(&name).await.unwrap();
    let (chat, asked) = fake_deepinfra().await;
    let admin = |args: &[&str]| {
        let (url, chat) = (url.clone(), chat.clone());
        let args: Vec<String> = args.iter().map(|s| s.to_string()).collect();
        async move {
            let refs: Vec<&str> = args.iter().map(String::as_str).collect();
            let (ok, text) = run(&url, &chat, &refs).await;
            assert!(ok, "admin {args:?} failed:\n{text}");
            text
        }
    };

    assert!(admin(&["filter", "list"]).await.contains("no rules"));
    for (i, n) in ["so bad", "xXgr1meXx", "Night", "Grime Time"].iter().enumerate() {
        store::create_user(&pool, &format!("zz-filter{i}@example.test"), "x", n).await.unwrap();
    }

    // A prompt, on at once; the test asks the model about each name once.
    let prompt = std::env::temp_dir().join(format!("pp-filter-test-{name}.txt"));
    std::fs::write(&prompt, "  the prompt\n").unwrap();
    let text = admin(&["filter", "add", "ai", "--file", prompt.to_str().unwrap(), "--on"]).await;
    assert!(text.contains("rule #1 added and on"), "{text}");
    let text = admin(&["filter", "test"]).await;
    assert_eq!(section(&text, "flagged now").len(), 1, "{text}");
    assert!(section(&text, "flagged now")[0].starts_with("name   \"so bad\""), "{text}");
    assert!(section(&text, "flagged now")[0].contains("by #1"), "{text}");
    assert_eq!(asked.load(Ordering::SeqCst), 4, "{text}");

    // A draft word: nothing changes for players; the test shows what it would flag.
    let text = admin(&["filter", "add", "word", "grime", "--note", "test word"]).await;
    assert!(text.contains("rule #2 added, off"), "{text}");
    let live = SlurFilter::new(&accounts::config::SlurFilterConfig::default(), pool.clone()).unwrap();
    assert!(!live.rules().await.unwrap().prompts.is_empty());
    assert_eq!(live.check(&live.rules().await.unwrap(), What::DisplayName, "xXgr1meXx").await.unwrap(), None);
    admin(&["filter", "sample", "add", "pass", "Grime Time"]).await;
    admin(&["filter", "sample", "add", "flag", "bad", "--what", "code"]).await;
    let text = admin(&["filter", "test", "2"]).await;
    let new = section(&text, "newly flagged");
    assert_eq!(new.len(), 2, "{text}");
    assert!(new[0].starts_with("name   \"Grime Time\"") && new[0].contains("sample: should pass"), "{text}");
    assert!(new[1].starts_with("name   \"xXgr1meXx\"") && new[1].contains("1 player, e.g. (no code)"), "{text}");
    assert_eq!(section(&text, "no longer flagged").len(), 0, "{text}");
    assert_eq!(section(&text, "samples wrong now").len(), 0, "{text}");
    assert_eq!(section(&text, "samples wrong with the change").len(), 1, "{text}");
    // Only the code sample was new to the prompt.
    assert_eq!(asked.load(Ordering::SeqCst), 5, "{text}");

    // An allow rule fixes the false positive; turning both on, and the prompt off, in one step.
    admin(&["filter", "add", "allow", "grime time"]).await;
    let text = admin(&["filter", "test", "2", "3", "--instead-of", "1"]).await;
    assert_eq!(section(&text, "samples wrong with the change").len(), 1, "the code sample: {text}");
    let gone = section(&text, "no longer flagged");
    assert_eq!(gone.len(), 2, "{text}");
    assert!(gone[0].starts_with("name   \"so bad\""), "{text}");
    assert!(gone[1].starts_with("code   \"bad\"") && gone[1].contains("sample: should flag"), "{text}");
    assert_eq!(section(&text, "newly flagged").len(), 1, "{text}");
    let text = admin(&["filter", "on", "2", "3"]).await;
    assert!(text.contains("on: #2, #3"), "{text}");
    let fresh = SlurFilter::new(&accounts::config::SlurFilterConfig::default(), pool.clone()).unwrap();
    let set = fresh.rules().await.unwrap();
    assert_eq!(set.local("xXgr1meXx"), sf::Local::Flagged(2));
    assert_eq!(set.local("GRIME TIME"), sf::Local::Allowed(3));

    // A rule that is on is changed only with --live.
    let (ok, text) = run(&url, &chat, &["filter", "edit", "2", "grimy"]).await;
    assert!(!ok && text.contains("--live"), "{text}");
    admin(&["filter", "edit", "2", "grimy", "--live"]).await;
    assert!(admin(&["filter", "show", "2"]).await.ends_with("---\ngrimy\n"));
    let (ok, text) = run(&url, &chat, &["filter", "add", "regex", "(oops"]).await;
    assert!(!ok && text.contains("bad regex"), "{text}");

    // Export and import: the rules already there are left alone.
    let export = admin(&["filter", "export"]).await;
    let file = std::env::temp_dir().join(format!("pp-filter-export-{name}.json"));
    std::fs::write(&file, &export).unwrap();
    let text = admin(&["filter", "import", file.to_str().unwrap()]).await;
    assert!(text.contains("0 rule(s) added (), 3 already there; 2 sample(s)"), "{text}");
    let _ = std::fs::remove_file(&file);
    let _ = std::fs::remove_file(&prompt);

    let audit: Vec<String> =
        sqlx::query_scalar("SELECT action FROM audit_log WHERE action LIKE 'filter.%' ORDER BY id")
            .fetch_all(&pool)
            .await
            .unwrap();
    assert_eq!(
        audit,
        [
            "filter.add",
            "filter.add",
            "filter.sample_add",
            "filter.sample_add",
            "filter.add",
            "filter.on",
            "filter.edit",
            "filter.import"
        ]
    );
}
