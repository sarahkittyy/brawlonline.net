//! Play keys, session tokens and other random secrets.
//!
//! **Play keys** authenticate Dolphin to the mm server (and later to game
//! reporting). Dolphin never sees the password or the launcher session.
//!
//! Slippi's launcher fetches the play key again on every Play
//! (`getUser` → `private.playKey`) and rewrites `user.json` from it, so the key
//! must be retrievable, not just verifiable. We therefore do not store the key
//! at all: it is derived as
//!
//! ```text
//! playKey = base64url( HMAC-SHA256(server_secret, "pp-playkey/v1|" uid "|" version) )
//! ```
//!
//! and the database stores only `play_key_version`. Rotating the key (password
//! change, admin action) increments the version. A database dump alone reveals
//! no key; the secret lives in the config of `accounts` and `mm` only.
//!
//! **Session tokens** (launcher login) and one-time tokens (password reset) are
//! 32 random bytes, base64url, stored as SHA-256 hashes.

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine as _;
use hmac::{Hmac, Mac};
use rand::RngCore;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use uuid::Uuid;

type HmacSha256 = Hmac<Sha256>;

/// Minimum accepted secret length in bytes.
pub const MIN_SECRET_LEN: usize = 32;

/// The server-side secret both `accounts` and `mm` use to derive play keys.
#[derive(Clone)]
pub struct PlayKeySecret(Vec<u8>);

impl std::fmt::Debug for PlayKeySecret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("PlayKeySecret(..)")
    }
}

#[derive(Debug, thiserror::Error)]
pub enum SecretError {
    #[error("play key secret must be hex or base64url and at least {MIN_SECRET_LEN} bytes")]
    Invalid,
}

impl PlayKeySecret {
    /// Parses a secret given as hex or base64url (as printed by `admin gen-secret`).
    pub fn parse(s: &str) -> Result<Self, SecretError> {
        let s = s.trim();
        let bytes = hex::decode(s)
            .ok()
            .or_else(|| URL_SAFE_NO_PAD.decode(s.trim_end_matches('=')).ok())
            .ok_or(SecretError::Invalid)?;
        if bytes.len() < MIN_SECRET_LEN {
            return Err(SecretError::Invalid);
        }
        Ok(PlayKeySecret(bytes))
    }

    pub fn from_bytes(bytes: &[u8]) -> Result<Self, SecretError> {
        if bytes.len() < MIN_SECRET_LEN {
            return Err(SecretError::Invalid);
        }
        Ok(PlayKeySecret(bytes.to_vec()))
    }

    /// A fresh random secret, hex encoded.
    pub fn generate_hex() -> String {
        let mut b = [0u8; 32];
        rand::rngs::OsRng.fill_bytes(&mut b);
        hex::encode(b)
    }

    fn mac(&self, uid: Uuid, version: i32) -> HmacSha256 {
        let mut mac = HmacSha256::new_from_slice(&self.0).expect("HMAC takes any key length");
        mac.update(b"pp-playkey/v1|");
        mac.update(uid.hyphenated().to_string().as_bytes());
        mac.update(b"|");
        mac.update(version.to_string().as_bytes());
        mac
    }

    /// The play key for this user and key version.
    pub fn derive(&self, uid: Uuid, version: i32) -> String {
        URL_SAFE_NO_PAD.encode(self.mac(uid, version).finalize().into_bytes())
    }

    /// Constant-time check of a play key presented by a client.
    pub fn verify(&self, uid: Uuid, version: i32, candidate: &str) -> bool {
        let expected = self.derive(uid, version);
        expected.as_bytes().ct_eq(candidate.as_bytes()).into()
    }
}

/// A random 32-byte token, base64url without padding.
pub fn random_token() -> String {
    let mut b = [0u8; 32];
    rand::rngs::OsRng.fill_bytes(&mut b);
    URL_SAFE_NO_PAD.encode(b)
}

/// SHA-256 of a token, for storage.
pub fn hash_token(token: &str) -> Vec<u8> {
    Sha256::digest(token.as_bytes()).to_vec()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn secret() -> PlayKeySecret {
        PlayKeySecret::parse(&"ab".repeat(32)).unwrap()
    }

    #[test]
    fn derive_is_stable_and_versioned() {
        let s = secret();
        let uid = Uuid::parse_str("0b6f5f3e-8f87-4d0c-9a51-6a3c2f1d9e10").unwrap();
        let k1 = s.derive(uid, 1);
        assert_eq!(k1, s.derive(uid, 1));
        assert_eq!(k1.len(), 43);
        assert_ne!(k1, s.derive(uid, 2));
        assert_ne!(k1, s.derive(Uuid::new_v4(), 1));
        assert!(s.verify(uid, 1, &k1));
        assert!(!s.verify(uid, 2, &k1));
        assert!(!s.verify(uid, 1, ""));
        assert!(!s.verify(uid, 1, &k1[..42]));
        let other = PlayKeySecret::parse(&"cd".repeat(32)).unwrap();
        assert!(!other.verify(uid, 1, &k1));
    }

    #[test]
    fn secret_parsing() {
        assert!(PlayKeySecret::parse("short").is_err());
        assert!(PlayKeySecret::parse(&"ab".repeat(31)).is_err());
        assert!(PlayKeySecret::parse(&PlayKeySecret::generate_hex()).is_ok());
        let b64 = URL_SAFE_NO_PAD.encode([7u8; 32]);
        assert!(PlayKeySecret::parse(&b64).is_ok());
    }

    #[test]
    fn tokens() {
        let a = random_token();
        let b = random_token();
        assert_ne!(a, b);
        assert_eq!(hash_token(&a), hash_token(&a));
        assert_eq!(hash_token(&a).len(), 32);
    }
}
