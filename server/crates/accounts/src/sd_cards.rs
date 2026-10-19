//! SD card listings for Other Games rooms (docs/design/brawl-mods.md §6.5,
//! `migrations/0015_sd_card_listings.sql`): a joiner's launcher tells its player how much of the
//! room's SD card differs, and which files, before Dolphin starts.
//!
//! - `PUT /v1/sd-cards/{root}/listing` (logged in), the listing as the body: a host's launcher,
//!   when it hosts. Stored once per root, after checking that its canonical manifest hashes
//!   (BLAKE2b-256) to `root` and that its file list is well formed, so nobody can put another
//!   card's files under a root. 201 when stored, 200 when the server already had it.
//! - `GET /v1/sd-cards/{root}/listing` (logged in): the listing, or 404 (a host on an older
//!   launcher; the joiner's launcher then asks without the sizes).
//!
//! A listing (DolphinTool `sdhash --listing`, `Machine::SDCard::EncodeCardListing`): a
//! little-endian u32 with the canonical manifest's length; the canonical manifest (the card's size
//! as a u64, then per 64 KiB chunk 0 for a chunk no file uses or 1 and its 16-byte hash); then the
//! file list (empty when not known): a u32 count, and per file its path's length, the path, its
//! size (two u32s) and its runs of chunks (a count, then first and count each). Listings unused for
//! 90 days are deleted at the next upload.

use axum::body::Body;
use axum::extract::{Path as UrlPath, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use blake2::digest::consts::U32;
use blake2::{Blake2b, Digest};

use crate::api::{current_user, limit};
use crate::error::{ApiError, ApiResult};
use crate::AppState;

const CHUNK_SIZE: u64 = 64 * 1024;
const MAX_CARD_SIZE: u64 = 64 << 30;
const MAX_CARD_FILES: u32 = 1 << 18;
const MAX_CARD_PATH: u32 = 1024;
const HASH_BYTES: usize = 16;
/// How long a listing nobody fetched or uploaded again is kept.
const UNUSED_DAYS: i32 = 90;

/// `root` as 64 lowercase hex digits, or None.
pub fn parse_root(root: &str) -> Option<[u8; 32]> {
    if root.len() != 64 || !root.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
        return None;
    }
    let mut out = [0u8; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&root[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

fn le32(b: &[u8], pos: usize) -> Option<u32> {
    b.get(pos..pos + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// Why a listing is refused (the message the launcher logs), or Ok when it is the card `root`'s.
pub fn check_listing(root: &[u8; 32], bytes: &[u8]) -> Result<(), &'static str> {
    let canon_len = le32(bytes, 0).ok_or("too short")? as usize;
    let canon = bytes.get(4..4usize.checked_add(canon_len).ok_or("too short")?).ok_or("too short")?;

    // The manifest: the card's size, then each chunk's kind and hash.
    if canon.len() < 8 {
        return Err("the manifest is too short");
    }
    let size = u64::from_le_bytes(canon[..8].try_into().unwrap());
    if size == 0 || size > MAX_CARD_SIZE {
        return Err("the card's size is not valid");
    }
    let chunks = size.div_ceil(CHUNK_SIZE) as u32;
    let mut pos = 8;
    for _ in 0..chunks {
        match canon.get(pos) {
            Some(0) => pos += 1,
            Some(1) if canon.len() - pos > HASH_BYTES => pos += 1 + HASH_BYTES,
            _ => return Err("the manifest is not valid"),
        }
    }
    if pos != canon.len() {
        return Err("the manifest is not valid");
    }
    let digest = Blake2b::<U32>::digest(canon);
    if digest.as_slice() != root {
        return Err("the manifest is not the card's");
    }

    check_file_list(&bytes[4 + canon_len..], chunks)
}

/// Machine::SDCard::DecodeFileList's checks: counts, paths, and every run inside the card.
fn check_file_list(list: &[u8], chunks: u32) -> Result<(), &'static str> {
    if list.is_empty() {
        return Ok(());
    }
    const BAD: &str = "the file list is not valid";
    let count = le32(list, 0).ok_or(BAD)?;
    if count > MAX_CARD_FILES {
        return Err(BAD);
    }
    let mut pos = 4usize;
    for _ in 0..count {
        let len = le32(list, pos).ok_or(BAD)?;
        pos += 4;
        if len == 0 || len > MAX_CARD_PATH {
            return Err(BAD);
        }
        let path = list.get(pos..pos + len as usize).ok_or(BAD)?;
        if path.iter().any(|&c| c < 0x20) {
            return Err(BAD);
        }
        pos += len as usize;
        // The size (two u32s), then the runs.
        le32(list, pos + 4).ok_or(BAD)?;
        pos += 8;
        let runs = le32(list, pos).ok_or(BAD)?;
        pos += 4;
        if runs > chunks || (list.len().saturating_sub(pos)) / 8 < runs as usize {
            return Err(BAD);
        }
        for _ in 0..runs {
            let first = le32(list, pos).ok_or(BAD)?;
            let n = le32(list, pos + 4).ok_or(BAD)?;
            pos += 8;
            if n == 0 || first >= chunks || n > chunks - first {
                return Err(BAD);
            }
        }
    }
    if pos != list.len() {
        return Err(BAD);
    }
    Ok(())
}

fn bad_root() -> ApiError {
    ApiError::bad_request("bad_root", "An SD card root is 64 lowercase hex digits")
}

pub async fn upload(
    State(state): State<AppState>,
    UrlPath(root): UrlPath<String>,
    headers: HeaderMap,
    body: Body,
) -> ApiResult<StatusCode> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.sd_listing_upload.check(&user.uid, now)).map_err(ApiError::rate_limited)?;
    let root_bytes = parse_root(&root).ok_or_else(bad_root)?;
    let max = state.cfg.sd_listing_max_bytes;
    let too_large = || {
        ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "too_large",
            format!("An SD card listing is at most {} MB.", max / (1024 * 1024)),
        )
    };
    let bytes =
        axum::body::to_bytes(body, usize::try_from(max).unwrap_or(usize::MAX)).await.map_err(|_| too_large())?;
    check_listing(&root_bytes, &bytes).map_err(|why| ApiError::bad_request("bad_listing", why))?;

    sqlx::query("DELETE FROM sd_card_listings WHERE used_at < now() - make_interval(days => $1)")
        .bind(UNUSED_DAYS)
        .execute(&state.pool)
        .await?;
    // Already here (another player hosts the same card): only marked as used.
    let known = sqlx::query("UPDATE sd_card_listings SET used_at = now() WHERE root = $1")
        .bind(&root)
        .execute(&state.pool)
        .await?;
    if known.rows_affected() == 1 {
        return Ok(StatusCode::OK);
    }
    let stored: i64 = sqlx::query_scalar("SELECT COALESCE(sum(bytes), 0)::bigint FROM sd_card_listings")
        .fetch_one(&state.pool)
        .await?;
    if stored as u64 + bytes.len() as u64 > state.cfg.sd_listings_max_total_bytes {
        tracing::warn!(stored, "SD card listing refused: SD_LISTINGS_MAX_TOTAL_BYTES reached");
        return Err(ApiError::new(
            StatusCode::INSUFFICIENT_STORAGE,
            "storage_full",
            "The server has no room for more SD card listings right now.",
        ));
    }
    let inserted = sqlx::query(
        "INSERT INTO sd_card_listings (root, listing, bytes, uploaded_by) VALUES ($1, $2, $3, $4)
         ON CONFLICT (root) DO NOTHING",
    )
    .bind(&root)
    .bind(bytes.as_ref())
    .bind(bytes.len() as i64)
    .bind(user.uid)
    .execute(&state.pool)
    .await?;
    Ok(if inserted.rows_affected() == 1 { StatusCode::CREATED } else { StatusCode::OK })
}

pub async fn download(
    State(state): State<AppState>,
    UrlPath(root): UrlPath<String>,
    headers: HeaderMap,
) -> ApiResult<Response> {
    let user = current_user(&state, &headers).await?;
    limit(&state, |l, now| l.sd_listing_fetch.check(&user.uid, now)).map_err(ApiError::rate_limited)?;
    parse_root(&root).ok_or_else(bad_root)?;
    let listing: Option<Vec<u8>> =
        sqlx::query_scalar("UPDATE sd_card_listings SET used_at = now() WHERE root = $1 RETURNING listing")
            .bind(&root)
            .fetch_optional(&state.pool)
            .await?;
    let listing = listing.ok_or_else(ApiError::not_found)?;
    Ok(([(header::CONTENT_TYPE, "application/octet-stream")], listing).into_response())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A listing of a card of `chunks` chunks (the last `zero` of them unused), with `list` as
    /// its file list, and its root.
    fn listing(chunks: u32, zero: u32, list: &[u8]) -> (Vec<u8>, [u8; 32]) {
        let mut canon = (u64::from(chunks) * CHUNK_SIZE).to_le_bytes().to_vec();
        for i in 0..chunks {
            if i >= chunks - zero {
                canon.push(0);
            } else {
                canon.push(1);
                canon.extend([i as u8; HASH_BYTES]);
            }
        }
        let root: [u8; 32] = Blake2b::<U32>::digest(&canon).into();
        let mut out = (canon.len() as u32).to_le_bytes().to_vec();
        out.extend(canon);
        out.extend(list);
        (out, root)
    }

    fn file_list(files: &[(&str, &[(u32, u32)])]) -> Vec<u8> {
        let mut out = (files.len() as u32).to_le_bytes().to_vec();
        for (path, runs) in files {
            out.extend((path.len() as u32).to_le_bytes());
            out.extend(path.as_bytes());
            out.extend(100u32.to_le_bytes());
            out.extend(0u32.to_le_bytes());
            out.extend((runs.len() as u32).to_le_bytes());
            for (first, n) in *runs {
                out.extend(first.to_le_bytes());
                out.extend(n.to_le_bytes());
            }
        }
        out
    }

    #[test]
    fn a_cards_own_listing_is_taken() {
        let list = file_list(&[("rex_/pf/a.pac", &[(0, 2)]), ("b.brstm", &[(5, 1)])]);
        let (bytes, root) = listing(8, 2, &list);
        assert_eq!(check_listing(&root, &bytes), Ok(()));
        let (bare, bare_root) = listing(8, 2, &[]);
        assert_eq!(check_listing(&bare_root, &bare), Ok(()), "no file list");
    }

    #[test]
    fn another_cards_listing_is_refused() {
        let (bytes, mut root) = listing(8, 0, &[]);
        root[0] ^= 1;
        assert_eq!(check_listing(&root, &bytes), Err("the manifest is not the card's"));
    }

    #[test]
    fn malformed_listings_are_refused() {
        let (bytes, root) = listing(4, 0, &file_list(&[("a", &[(0, 1)])]));
        assert!(check_listing(&root, &bytes[..3]).is_err());
        assert!(check_listing(&root, &bytes[..bytes.len() - 1]).is_err(), "list cut short");
        let mut kind = bytes.clone();
        kind[4 + 8] = 7;
        assert!(check_listing(&root, &kind).is_err(), "unknown chunk kind");
        let (past, past_root) = listing(4, 0, &file_list(&[("a", &[(3, 2)])]));
        assert!(check_listing(&past_root, &past).is_err(), "a run past the card");
        let (ctl, ctl_root) = listing(4, 0, &file_list(&[("a\nb", &[(0, 1)])]));
        assert!(check_listing(&ctl_root, &ctl).is_err(), "a control character in a path");
        let mut long = bytes.clone();
        long[..4].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(check_listing(&root, &long).is_err(), "a manifest longer than the listing");
    }

    #[test]
    fn roots_are_lowercase_hex() {
        assert!(parse_root(&"ab".repeat(32)).is_some());
        assert!(parse_root(&"AB".repeat(32)).is_none());
        assert!(parse_root("abc").is_none());
        assert!(parse_root(&"zz".repeat(32)).is_none());
    }
}
