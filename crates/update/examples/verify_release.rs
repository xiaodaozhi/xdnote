//! Read-only authentication using the same compiled production root as updates.
use sha2::{Digest, Sha256};
use std::{
    fs::File,
    io::{self, Read},
};

fn bounded(reader: impl Read, limit: usize) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    reader.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > limit {
        return Err(io::Error::from(io::ErrorKind::InvalidData));
    }
    Ok(bytes)
}

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let root_sha = format!(
        "{:x}",
        Sha256::digest(xdnote_update::PRODUCTION_KEYRING.as_bytes())
    );
    let result = (|| -> Result<(String, String), &'static str> {
        if args.len() != 2 {
            return Err("expected_manifest_and_signature_paths");
        }
        let bytes = bounded(
            File::open(&args[0]).map_err(|_| "manifest_input")?,
            xdnote_update::MANIFEST_LIMIT,
        )
        .map_err(|_| "manifest_input")?;
        let signature = bounded(
            File::open(&args[1]).map_err(|_| "signature_input")?,
            xdnote_update::SIGNATURE_LIMIT,
        )
        .map_err(|_| "signature_input")?;
        let envelope = std::str::from_utf8(&signature).map_err(|_| "signature_input")?;
        Ok((
            format!("{:x}", Sha256::digest(&bytes)),
            xdnote_update::verify_manifest(&bytes, envelope, xdnote_update::PRODUCTION_KEYRING),
        ))
    })();
    let (manifest_sha, error) = match result {
        Ok(value) => value,
        Err(error) => (String::new(), error.into()),
    };
    let ok = error.is_empty();
    println!(
        "{}",
        serde_json::json!({"ok": ok, "authenticated": ok, "error": error,
        "manifestSha256": manifest_sha, "trustRootSha256": root_sha,
        "scope": "original manifest bytes and compiled public root only; no package/install/license/release acceptance"})
    );
    if !ok {
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn metadata_reads_are_bounded_before_parsing() {
        assert_eq!(bounded(&b"exact"[..], 5).unwrap(), b"exact");
        assert!(bounded(&b"oversized"[..], 5).is_err());
    }
}
