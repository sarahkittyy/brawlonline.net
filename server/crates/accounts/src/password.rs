//! Argon2id password hashing (design section 3: m = 64 MiB, t = 3, p = 1).
//! Hashes are PHC strings, so the parameters travel with each hash and can be
//! raised later without invalidating old ones.

use argon2::password_hash::{rand_core::OsRng, PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::{Algorithm, Argon2, Params, Version};

pub const MIN_PASSWORD_LEN: usize = 8;
pub const MAX_PASSWORD_LEN: usize = 256;

#[derive(Debug, Clone, Copy)]
pub struct HashParams {
    pub memory_kib: u32,
    pub iterations: u32,
}

impl HashParams {
    fn argon2(self) -> Argon2<'static> {
        let params = Params::new(self.memory_kib, self.iterations, 1, None).expect("valid argon2 params");
        Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
    }
}

pub fn validate_password(pw: &str) -> Result<(), String> {
    let n = pw.chars().count();
    if n < MIN_PASSWORD_LEN {
        return Err(format!("Password must be at least {MIN_PASSWORD_LEN} characters"));
    }
    if n > MAX_PASSWORD_LEN {
        return Err(format!("Password must be at most {MAX_PASSWORD_LEN} characters"));
    }
    Ok(())
}

pub fn hash(params: HashParams, pw: &str) -> anyhow::Result<String> {
    let salt = SaltString::generate(&mut OsRng);
    params
        .argon2()
        .hash_password(pw.as_bytes(), &salt)
        .map(|h| h.to_string())
        .map_err(|e| anyhow::anyhow!("argon2: {e}"))
}

/// Verifies against a stored PHC string (any argon2 parameters).
pub fn verify(pw: &str, stored: &str) -> bool {
    match PasswordHash::new(stored) {
        Ok(parsed) => Argon2::default().verify_password(pw.as_bytes(), &parsed).is_ok(),
        Err(_) => false,
    }
}

/// Hashes on the blocking pool so a 64 MiB hash does not stall the runtime.
pub async fn hash_async(params: HashParams, pw: String) -> anyhow::Result<String> {
    tokio::task::spawn_blocking(move || hash(params, &pw)).await?
}

pub async fn verify_async(pw: String, stored: String) -> bool {
    tokio::task::spawn_blocking(move || verify(&pw, &stored)).await.unwrap_or(false)
}

/// A fixed hash to verify against when the account does not exist, so a
/// login for an unknown email takes as long as one with a wrong password.
pub fn dummy_hash(params: HashParams) -> String {
    hash(params, "dummy password for timing").expect("hash")
}

#[cfg(test)]
mod tests {
    use super::*;

    const FAST: HashParams = HashParams { memory_kib: 1024, iterations: 1 };

    #[test]
    fn hash_and_verify() {
        let h = hash(FAST, "correct horse").unwrap();
        assert!(h.starts_with("$argon2id$v=19$m=1024,t=1,p=1$"));
        assert!(verify("correct horse", &h));
        assert!(!verify("wrong horse", &h));
        assert!(!verify("correct horse", "not a phc string"));
        assert_ne!(h, hash(FAST, "correct horse").unwrap(), "salted");
    }

    #[test]
    fn design_parameters_are_encoded() {
        let h = hash(HashParams { memory_kib: 65536, iterations: 3 }, "pw").unwrap();
        assert!(h.starts_with("$argon2id$v=19$m=65536,t=3,p=1$"));
        assert!(verify("pw", &h));
    }

    #[test]
    fn password_rules() {
        assert!(validate_password("1234567").is_err());
        assert!(validate_password("12345678").is_ok());
        assert!(validate_password(&"x".repeat(257)).is_err());
    }
}
