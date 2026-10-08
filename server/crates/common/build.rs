// `sqlx::migrate!` embeds server/migrations at build time but does not tell Cargo to
// watch the folder, so a new migration file alone would not trigger a rebuild (for
// example in CI's incremental target dir). This does.
fn main() {
    println!("cargo:rerun-if-changed=../../migrations");
}
