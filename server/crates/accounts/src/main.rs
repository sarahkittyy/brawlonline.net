use clap::Parser;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Secrets come from the environment; in development from server/.env.
    let _ = dotenvy::dotenv();
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info,sqlx=warn".into()),
        )
        .init();
    accounts::run(accounts::Config::parse()).await
}
