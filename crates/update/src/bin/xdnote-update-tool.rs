//! Release-only Ed25519 signing and public verification on the frozen candidate bytes.
use base64ct::{Base64, Encoding};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use serde::Deserialize;
use serde_json::json;
use sha2::{Digest, Sha256};
use std::{
    env,
    ffi::OsStr,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::Path,
};
use xdnote_update::{MANIFEST_LIMIT, PACKAGE_LIMIT, PRODUCTION_KEYRING, SIGNATURE_LIMIT};
use zeroize::Zeroizing;

type Result<T> = std::result::Result<T, &'static str>;
const SIGNATURE_MARKER: &str = "<!-- sparkle-signatures:";
const SIGNATURE_PREFIX: &str = "<!-- sparkle-signatures:\n";
const SPARKLE_PUBLIC: &str = include_str!("../../../../tools/sparkle-update-key.json");

fn text(value: &OsStr) -> Result<&str> {
    value.to_str().ok_or("invalid_arguments")
}

fn read(path: &OsStr, limit: u64) -> Result<Vec<u8>> {
    let metadata = fs::symlink_metadata(path).map_err(|_| "input_unavailable")?;
    if !metadata.is_file() || metadata.len() > limit {
        return Err("invalid_input_file");
    }
    let mut bytes = Vec::new();
    File::open(path)
        .map_err(|_| "input_unavailable")?
        .take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "input_read_failed")?;
    if bytes.len() as u64 > limit {
        return Err("invalid_input_file");
    }
    Ok(bytes)
}

fn write(path: &OsStr, bytes: &[u8]) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|_| "output_unavailable")?;
    let result = file.write_all(bytes).and_then(|_| file.sync_all());
    drop(file);
    if result.is_err() {
        let _ = fs::remove_file(path);
        return Err("output_write_failed");
    }
    Ok(())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn signing_key(variable: &str, public: &VerifyingKey) -> Result<SigningKey> {
    let source = Zeroizing::new(env::var(variable).map_err(|_| "signing_key_not_configured")?);
    if source.len() != 64
        || !source
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err("invalid_signing_key");
    }
    let mut seed = Zeroizing::new([0u8; 32]);
    for (index, byte) in seed.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&source[index * 2..index * 2 + 2], 16)
            .map_err(|_| "invalid_signing_key")?;
    }
    let key = SigningKey::from_bytes(&seed);
    if &key.verifying_key() != public {
        return Err("signing_key_public_mismatch");
    }
    Ok(key)
}

fn manifest_signing_key() -> Result<(String, SigningKey)> {
    let id = env::var("XDNOTE_UPDATE_KEY_ID").map_err(|_| "signing_key_id_not_configured")?;
    let public = xdnote_update::production_manifest_key(&id)?;
    let key = signing_key("XDNOTE_UPDATE_SIGNING_KEY_HEX", &public)?;
    Ok((id, key))
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct SparklePublicKey {
    schema_version: u32,
    public_key: String,
}

fn sparkle_public_key() -> Result<VerifyingKey> {
    let config: SparklePublicKey =
        serde_json::from_str(SPARKLE_PUBLIC).map_err(|_| "invalid_sparkle_public_key")?;
    if config.schema_version != 1 {
        return Err("invalid_sparkle_public_key");
    }
    let raw: [u8; 32] = Base64::decode_vec(&config.public_key)
        .map_err(|_| "invalid_sparkle_public_key")?
        .try_into()
        .map_err(|_| "invalid_sparkle_public_key")?;
    let public = VerifyingKey::from_bytes(&raw).map_err(|_| "invalid_sparkle_public_key")?;
    if public.is_weak() {
        return Err("invalid_sparkle_public_key");
    }
    Ok(public)
}

fn sparkle_signing_key() -> Result<SigningKey> {
    signing_key("XDNOTE_SPARKLE_SIGNING_KEY_HEX", &sparkle_public_key()?)
}

fn signature(encoded: &str) -> Result<Signature> {
    let raw: [u8; 64] = Base64::decode_vec(encoded)
        .map_err(|_| "invalid_sparkle_signature")?
        .try_into()
        .map_err(|_| "invalid_sparkle_signature")?;
    Ok(Signature::from_bytes(&raw))
}

fn verify_appcast(bytes: &[u8], public: &VerifyingKey) -> Result<()> {
    let text = std::str::from_utf8(bytes).map_err(|_| "invalid_appcast_encoding")?;
    let (content, block) = text
        .rsplit_once(SIGNATURE_PREFIX)
        .ok_or("appcast_signature_missing")?;
    if !content.starts_with("<?xml") || content.contains(SIGNATURE_MARKER) {
        return Err("invalid_appcast_content");
    }
    let (line, tail) = block.split_once('\n').ok_or("invalid_appcast_signature")?;
    let encoded = line
        .strip_prefix("edSignature: ")
        .ok_or("invalid_appcast_signature")?;
    if tail != format!("length: {}\n-->\n", content.len()) {
        return Err("invalid_appcast_signature_length");
    }
    public
        .verify_strict(content.as_bytes(), &signature(encoded)?)
        .map_err(|_| "appcast_signature_mismatch")
}

fn signed_appcast(mut bytes: Vec<u8>, key: &SigningKey) -> Result<Vec<u8>> {
    let text = std::str::from_utf8(&bytes).map_err(|_| "invalid_appcast_encoding")?;
    if !text.starts_with("<?xml") || text.contains(SIGNATURE_MARKER) {
        return Err("invalid_unsigned_appcast");
    }
    let signature = Base64::encode_string(&key.sign(&bytes).to_bytes());
    let block = format!(
        "{SIGNATURE_PREFIX}edSignature: {signature}\nlength: {}\n-->\n",
        bytes.len()
    );
    bytes.extend_from_slice(block.as_bytes());
    verify_appcast(&bytes, &key.verifying_key())?;
    Ok(bytes)
}

fn sign_archive(archive: &OsStr, version: &OsStr, build: &OsStr, output: &OsStr) -> Result<()> {
    let version = text(version)?;
    let build: u32 = text(build)?
        .parse()
        .map_err(|_| "invalid_archive_identity")?;
    let filename = format!("xdnote-{version}-macos-arm64.app.zip");
    if !(1..=65535).contains(&build)
        || xdnote_update::compare_versions(version, version) != 0
        || version.contains("-dev.")
        || Path::new(archive).file_name() != Some(OsStr::new(&filename))
    {
        return Err("invalid_archive_identity");
    }
    let bytes = read(archive, PACKAGE_LIMIT)?;
    if bytes.is_empty() {
        return Err("invalid_archive_file");
    }
    let key = sparkle_signing_key()?;
    let signature = key.sign(&bytes);
    key.verifying_key()
        .verify_strict(&bytes, &signature)
        .map_err(|_| "archive_signature_mismatch")?;
    let receipt = json!({"schema_version": 1, "version": version, "build": build,
        "filename": filename, "bytes": bytes.len(), "sha256": hex(&Sha256::digest(&bytes)),
        "ed_signature": Base64::encode_string(&signature.to_bytes())});
    write(
        output,
        &serde_json::to_vec(&receipt).map_err(|_| "output_serialization_failed")?,
    )
}

fn verify_manifest(bytes: &[u8], envelope: &[u8]) -> Result<()> {
    let envelope = std::str::from_utf8(envelope).map_err(|_| "invalid_signature_envelope")?;
    if xdnote_update::verify_manifest(bytes, envelope, PRODUCTION_KEYRING).is_empty() {
        Ok(())
    } else {
        Err("manifest_authentication_failed")
    }
}

fn run() -> Result<()> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    match args.first().and_then(|argument| argument.to_str()) {
        Some("check-signing-keys") if args.len() == 1 => {
            let (_, manifest) = manifest_signing_key()?;
            if manifest.verifying_key() == sparkle_signing_key()?.verifying_key() {
                return Err("signing_keys_must_be_distinct");
            }
        }
        Some("sign-archive") if args.len() == 5 => {
            sign_archive(&args[1], &args[2], &args[3], &args[4])?
        }
        Some("verify-archive") if args.len() == 3 => sparkle_public_key()?
            .verify_strict(
                &read(&args[1], PACKAGE_LIMIT)?,
                &signature(text(&args[2])?)?,
            )
            .map_err(|_| "archive_signature_mismatch")?,
        Some("sign-appcast") if args.len() == 3 => write(
            &args[2],
            &signed_appcast(
                read(&args[1], MANIFEST_LIMIT as u64)?,
                &sparkle_signing_key()?,
            )?,
        )?,
        Some("verify-appcast") if args.len() == 2 => verify_appcast(
            &read(&args[1], MANIFEST_LIMIT as u64 + 512)?,
            &sparkle_public_key()?,
        )?,
        Some("sign") if args.len() == 3 => {
            let bytes = read(&args[1], MANIFEST_LIMIT as u64)?;
            let (id, key) = manifest_signing_key()?;
            let envelope = serde_json::to_vec(&json!({"schema_version": 1, "algorithm": "ed25519",
                "key_id": id, "signature": hex(&key.sign(&bytes).to_bytes())}))
            .map_err(|_| "output_serialization_failed")?;
            verify_manifest(&bytes, &envelope)?;
            write(&args[2], &envelope)?;
        }
        Some("verify") if args.len() == 3 => verify_manifest(
            &read(&args[1], MANIFEST_LIMIT as u64)?,
            &read(&args[2], SIGNATURE_LIMIT as u64)?,
        )?,
        _ => return Err("invalid_arguments"),
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signed_feed_binds_raw_bytes_and_has_one_signature_block() {
        let key = SigningKey::from_bytes(&[42; 32]);
        let content = b"<?xml version='1.0'?>\n<rss/>\n";
        let mut signed = signed_appcast(content.to_vec(), &key).unwrap();
        assert!(signed.starts_with(content));
        assert!(verify_appcast(&signed, &key.verifying_key()).is_ok());
        assert!(signed_appcast(signed.clone(), &key).is_err());
        signed[content.len() - 5] ^= 1;
        assert_eq!(
            verify_appcast(&signed, &key.verifying_key()),
            Err("appcast_signature_mismatch")
        );
    }
}
