use super::*;
use ed25519_dalek::{Signer, SigningKey};
use serde_json::{json, Value};

fn candidate() -> Value {
    let version = "0.1.0-alpha.7";
    let mut assets = Vec::new();
    for (platform, architecture, package_type) in TARGETS {
        let (suffix, requirements) = target(platform, architecture, package_type).unwrap();
        let filename = format!("xdnote-{version}-{suffix}");
        assets.push(json!({"filename":filename,"platform":platform,"architecture":architecture,"package_type":package_type,
            "minimum_system":requirements,"bytes":12,"sha256":"ab".repeat(32),
            "url":format!("https://github.com/{REPOSITORY}/releases/download/v{version}/{filename}")}));
    }
    json!({"schema_version":1,"product":"xdnote","version":version,"channel":"alpha","build":20,
        "tag":format!("v{version}"),"commit":"a".repeat(40),"repository":REPOSITORY,"assets":assets,
        "macos_appcast_url":format!("https://github.com/{REPOSITORY}/releases/download/v{version}/appcast.xml")})
}

fn inspect(value: &Value) -> Inspection {
    inspect_release(
        &value.to_string(),
        "0.1.0-alpha.6",
        19,
        "alpha",
        "windows",
        "x86_64",
        "exe",
    )
}

#[test]
fn semantic_and_channel_progression() {
    assert_eq!(compare_versions("0.1.0-alpha.9", "0.1.0-alpha.10"), -1);
    assert_eq!(compare_versions("0.1.0-dev.99", "0.1.0-alpha.1"), -1);
    assert_eq!(compare_versions("0.1.0-rc.1", "0.1.0"), -1);
    for invalid in [
        "v0.1.0",
        "0.1.0-alpha.0",
        "0.1.0-alpha.01",
        "0.1.0-alpha.1+metadata",
        "0.1.0-preview.1",
        "65536.0.0",
    ] {
        assert_eq!(compare_versions(invalid, "0.1.0"), 2, "{invalid}");
    }
    for (current, channel, tag, expected) in [
        ("0.1.0-alpha.6", "alpha", "v0.1.0-beta.1", true),
        ("0.1.0-beta.1", "beta", "v0.2.0-alpha.1", false),
        ("0.1.0-rc.1", "rc", "v0.2.0-beta.1", false),
        ("0.1.0", "stable", "v0.2.0-rc.1", false),
        ("0.1.0", "stable", "v0.2.0", true),
        ("0.1.0-dev.99", "development", "v0.1.0-alpha.1", true),
        ("0.1.0-alpha.6", "stable", "v0.1.0", false),
    ] {
        assert_eq!(
            release_tag_eligible(tag, current, channel),
            expected,
            "{current}/{channel}/{tag}"
        );
    }
}

#[test]
fn complete_candidate_and_native_build_are_bound() {
    let value = candidate();
    let result = inspect(&value);
    assert!(result.valid && result.newer);
    assert!(result.filename.ends_with("-setup.exe"));
    assert!(result.appcast_url.ends_with("/v0.1.0-alpha.7/appcast.xml"));
    assert!(
        !inspect_release(
            &value.to_string(),
            "0.1.0-alpha.7",
            20,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .newer
    );
    assert_eq!(
        inspect_release(
            &value.to_string(),
            "0.1.0-alpha.6",
            20,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .error,
        "non_monotonic_build"
    );
    assert_eq!(
        inspect_release(
            &value.to_string(),
            "0.1.0-alpha.6",
            19,
            "alpha",
            "windows",
            "arm64",
            "exe"
        )
        .error,
        "unsupported_installation"
    );
}

#[test]
fn metadata_mutations_fail_closed() {
    let original = candidate();
    let mut mutations = Vec::new();
    let mut v = original.clone();
    v["assets"].as_array_mut().unwrap().pop();
    mutations.push(v);
    let mut v = original.clone();
    v["repository"] = json!("attacker/xdnote");
    mutations.push(v);
    let mut v = original.clone();
    v["assets"][0]["url"] = json!("https://example.com/package");
    mutations.push(v);
    let mut v = original.clone();
    v["assets"][0]["bytes"] = json!(true);
    mutations.push(v);
    let mut v = original.clone();
    v["assets"][0]["minimum_system"]["windows"] = json!("11");
    mutations.push(v);
    let mut v = original.clone();
    v["assets"][0]["sha256"] = json!("AB".repeat(32));
    mutations.push(v);
    let mut v = original.clone();
    v["assets"][1] = v["assets"][0].clone();
    mutations.push(v);
    let mut v = original.clone();
    v["channel"] = json!("beta");
    mutations.push(v);
    let mut v = original.clone();
    v["macos_appcast_url"] =
        json!("https://github.com/xiaodaozhi/xdnote/releases/latest/download/appcast.xml");
    mutations.push(v);
    for v in mutations {
        assert!(!inspect(&v).valid, "{v}");
    }
    let duplicate = original
        .to_string()
        .replacen("\"build\":20", "\"build\":20,\"build\":21", 1);
    assert!(
        !inspect_release(
            &duplicate,
            "0.1.0-alpha.6",
            19,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .valid
    );
    assert!(
        !inspect_release(
            &" ".repeat(MANIFEST_LIMIT + 1),
            "0.1.0",
            1,
            "stable",
            "windows",
            "x86_64",
            "exe"
        )
        .valid
    );
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn publisher_signature_authenticates_exact_bytes_and_local_key() {
    // This deterministic key exists only inside unit tests, never production resources.
    let key = SigningKey::from_bytes(&[42; 32]);
    let bytes = candidate().to_string().into_bytes();
    let envelope = json!({"schema_version":1,"algorithm":"ed25519","key_id":"fixture-only",
        "signature":hex(&key.sign(&bytes).to_bytes())})
    .to_string();
    let keyring = json!({"schema_version":1,"keys":[{"key_id":"fixture-only","public_key":hex(key.verifying_key().as_bytes())}]}).to_string();
    assert_eq!(verify_manifest(&bytes, &envelope, &keyring), "");
    let mut changed = bytes.clone();
    changed.push(b' ');
    assert_eq!(
        verify_manifest(&changed, &envelope, &keyring),
        "signature_mismatch"
    );
    assert_eq!(
        verify_manifest(&bytes, &envelope, "{\"schema_version\":1,\"keys\":[]}"),
        "untrusted_signing_key"
    );
    let mut duplicate: Value = serde_json::from_str(&keyring).unwrap();
    let first = duplicate["keys"][0].clone();
    duplicate["keys"].as_array_mut().unwrap().push(first);
    assert_eq!(
        verify_manifest(&bytes, &envelope, &duplicate.to_string()),
        "invalid_trust_keyring"
    );
    assert!(!verify_manifest(&bytes, &envelope.replace("ed25519", "other"), &keyring).is_empty());
    let weak =
        json!({"schema_version":1,"keys":[{"key_id":"fixture-only","public_key":"00".repeat(32)}]});
    assert_eq!(
        verify_manifest(&bytes, &envelope, &weak.to_string()),
        "invalid_trust_keyring"
    );
}

#[test]
fn all_supported_platform_profiles_bind_their_own_package_and_minimum() {
    let value = candidate();
    for (platform, arch, kind, suffix, minimum) in [
        (
            "windows",
            "x86_64",
            "zip",
            "windows-x86_64.zip",
            json!({"windows":"10.0.17763"}),
        ),
        (
            "windows",
            "x86_64",
            "exe",
            "windows-x86_64-setup.exe",
            json!({"windows":"10.0.17763"}),
        ),
        (
            "macos",
            "arm64",
            "app.zip",
            "macos-arm64.app.zip",
            json!({"macos":"14.0"}),
        ),
        (
            "macos",
            "arm64",
            "dmg",
            "macos-arm64.dmg",
            json!({"macos":"14.0"}),
        ),
        (
            "linux",
            "x86_64",
            "tar.gz",
            "linux-x86_64.tar.gz",
            json!({"distribution":"ubuntu","version":"22.04","glibc":"2.35","display_server":"Wayland/X11"}),
        ),
        (
            "linux",
            "x86_64",
            "deb",
            "ubuntu22.04-x86_64.deb",
            json!({"distribution":"ubuntu","version":"22.04","glibc":"2.35","display_server":"Wayland/X11"}),
        ),
    ] {
        let result = inspect_release(
            &value.to_string(),
            "0.1.0-alpha.6",
            19,
            "alpha",
            platform,
            arch,
            kind,
        );
        assert!(result.valid && result.newer, "{result:?}");
        assert_eq!(result.filename, format!("xdnote-0.1.0-alpha.7-{suffix}"));
        assert_eq!(result.commit, "a".repeat(40));
        let asset = value["assets"]
            .as_array()
            .unwrap()
            .iter()
            .find(|a| a["filename"] == result.filename)
            .unwrap();
        assert_eq!(asset["minimum_system"], minimum);
    }
    for (platform, arch, kind) in [
        ("windows", "x86_64", "msix"),
        ("windows", "arm64", "exe"),
        ("linux", "arm64", "deb"),
        ("macos", "x86_64", "app.zip"),
        ("macos", "x86_64", "dmg"),
    ] {
        assert_eq!(
            inspect_release(
                &value.to_string(),
                "0.1.0-alpha.6",
                19,
                "alpha",
                platform,
                arch,
                kind
            )
            .error,
            "unsupported_installation"
        );
    }
    let mut cli = value.clone();
    cli["component"] = json!("cli");
    assert_eq!(inspect(&cli).error, "unsupported_manifest");
    let mut old_macos = value.clone();
    for asset in old_macos["assets"].as_array_mut().unwrap() {
        if asset["platform"] == "macos" {
            asset["minimum_system"]["macos"] = json!("13.0");
        }
    }
    assert_eq!(inspect(&old_macos).error, "asset_identity_mismatch");
}

#[test]
fn actual_development_identity_has_no_invented_release_counter() {
    for current in ["0.1.0-dev.1dd828e0ce69", "0.1.0-dev.012345678901"] {
        let result = inspect_release(
            &candidate().to_string(),
            current,
            0,
            "development",
            "macos",
            "arm64",
            "app.zip",
        );
        assert!(result.valid && result.newer, "{result:?}");
    }
    assert_eq!(
        compare_versions("0.1.0-dev.aaaaaaaaaaaa", "0.1.0-dev.bbbbbbbbbbbb"),
        0
    );
    let mut public_dev = candidate();
    public_dev["version"] = json!("0.1.0-dev.1dd828e0ce69");
    assert_eq!(inspect(&public_dev).error, "invalid_release_version");
    assert_eq!(
        inspect_release(
            &candidate().to_string(),
            "0.1.0-alpha.6",
            0,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .error,
        "invalid_current_identity"
    );
}

#[test]
fn bounds_schema_identity_and_duplicate_fields_fail_closed() {
    let original = candidate();
    for (field, replacement) in [
        ("schema_version", json!(2)),
        ("product", json!("other")),
        ("tag", json!("v0.1.0-alpha.8")),
        ("commit", json!("a".repeat(39))),
        ("build", json!(0)),
        ("build", json!(65536)),
        ("extra", json!(true)),
    ] {
        let mut value = original.clone();
        value[field] = replacement;
        assert!(!inspect(&value).valid, "{value}");
    }
    for bytes in [0, PACKAGE_LIMIT + 1] {
        let mut value = original.clone();
        value["assets"][0]["bytes"] = json!(bytes);
        assert_eq!(inspect(&value).error, "asset_identity_mismatch");
    }
    let mut boundary = original.clone();
    boundary["assets"][0]["bytes"] = json!(PACKAGE_LIMIT);
    assert!(inspect(&boundary).valid);
    let nested_duplicate = original.to_string().replacen(
        "\"platform\":\"windows\"",
        "\"platform\":\"windows\",\"platform\":\"linux\"",
        1,
    );
    assert_eq!(
        inspect_release(
            &nested_duplicate,
            "0.1.0-alpha.6",
            19,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .error,
        "invalid_manifest_json"
    );
}

fn signed(value: &Value) -> (Vec<u8>, String, String) {
    // This deterministic signing key exists only in the unit-test binary.
    let key = SigningKey::from_bytes(&[83; 32]);
    let bytes = value.to_string().into_bytes();
    let envelope = json!({"schema_version":1,"algorithm":"ed25519","key_id":"test-only",
        "signature":hex(&key.sign(&bytes).to_bytes())})
    .to_string();
    let keyring = json!({"schema_version":1,"keys":[{"key_id":"test-only","public_key":hex(key.verifying_key().as_bytes())}]}).to_string();
    (bytes, envelope, keyring)
}

fn authenticated(value: &Value) -> AuthenticatedRelease {
    let (bytes, envelope, keyring) = signed(value);
    authenticate_with_keyring(
        &bytes,
        &envelope,
        "0.1.0-alpha.6",
        19,
        "alpha",
        "windows",
        "x86_64",
        "exe",
        &keyring,
    )
    .unwrap()
}

#[test]
fn production_root_cannot_authenticate_the_fixture() {
    let (bytes, envelope, _) = signed(&candidate());
    assert_eq!(
        serde_json::from_str::<Value>(PRODUCTION_KEYRING).unwrap(),
        json!({"schema_version":1,"keys":[]})
    );
    assert_eq!(
        authenticate_release(
            &bytes,
            &envelope,
            "0.1.0-alpha.6",
            19,
            "alpha",
            "windows",
            "x86_64",
            "exe"
        )
        .unwrap_err(),
        "untrusted_signing_key"
    );
}

#[test]
fn authentication_cannot_bypass_version_or_build_gates() {
    let (bytes, envelope, keyring) = signed(&candidate());
    for (version, build, error) in [
        ("0.1.0-alpha.7", 20, "no_eligible_update"),
        ("0.1.0-alpha.6", 20, "non_monotonic_build"),
    ] {
        assert_eq!(
            authenticate_with_keyring(
                &bytes, &envelope, version, build, "alpha", "windows", "x86_64", "exe", &keyring
            )
            .unwrap_err(),
            error
        );
    }
    let release = authenticated(&candidate());
    assert_eq!(release.inspection().build, 20);
}

#[test]
fn signature_bounds_invalid_utf8_wrong_keys_and_tampering_fail_closed() {
    let (bytes, envelope, keyring) = signed(&candidate());
    assert_eq!(
        verify_manifest(&[0xff], &envelope, &keyring),
        "invalid_manifest_json"
    );
    assert_eq!(
        verify_manifest(&vec![b' '; MANIFEST_LIMIT + 1], &envelope, &keyring),
        "manifest_too_large"
    );
    assert_eq!(
        verify_manifest(&bytes, &" ".repeat(SIGNATURE_LIMIT + 1), &keyring),
        "signature_metadata_too_large"
    );
    assert_eq!(
        verify_manifest(&bytes, &envelope, &" ".repeat(KEYRING_LIMIT + 1)),
        "signature_metadata_too_large"
    );
    let mut changed: Value = serde_json::from_str(&envelope).unwrap();
    changed["signature"] = json!("00".repeat(64));
    assert_eq!(
        verify_manifest(&bytes, &changed.to_string(), &keyring),
        "signature_mismatch"
    );
    changed["signature"] = json!("ff".repeat(63));
    assert_eq!(
        verify_manifest(&bytes, &changed.to_string(), &keyring),
        "invalid_signature"
    );
    assert_eq!(
        verify_manifest(
            &bytes,
            &envelope,
            &keyring.replace("test-only", "other-key")
        ),
        "untrusted_signing_key"
    );
    let duplicate = envelope.replace(
        "\"schema_version\":1",
        "\"schema_version\":1,\"schema_version\":1",
    );
    assert_eq!(
        verify_manifest(&bytes, &duplicate, &keyring),
        "invalid_signature_envelope"
    );
    let extra = keyring.replacen('{', "{\"extra\":1,", 1);
    assert_eq!(
        verify_manifest(&bytes, &envelope, &extra),
        "invalid_trust_keyring"
    );
}

#[test]
fn authenticated_package_stream_rejects_truncation_overflow_and_corruption() {
    let package = vec![b'x'; 150_000];
    let mut value = candidate();
    value["assets"][1]["bytes"] = json!(package.len());
    value["assets"][1]["sha256"] = json!(hex(&Sha256::digest(&package)));
    let release = authenticated(&value);
    assert_eq!(release.verify_package(package.as_slice()), Ok(()));
    assert_eq!(
        release.verify_package(&package[..package.len() - 1]),
        Err("package_length_mismatch")
    );
    let mut long = package.clone();
    long.push(0);
    let mut cursor = std::io::Cursor::new(long);
    assert_eq!(
        release.verify_package(&mut cursor),
        Err("package_length_mismatch")
    );
    assert_eq!(cursor.position(), package.len() as u64 + 1);
    let mut corrupt = package.clone();
    corrupt[100_000] ^= 1;
    assert_eq!(
        release.verify_package(corrupt.as_slice()),
        Err("package_digest_mismatch")
    );
    assert_eq!(
        release.verify_package(&b""[..]),
        Err("package_length_mismatch")
    );
}

#[test]
fn package_stream_handles_short_reads_interrupts_and_io_failure() {
    struct Reader {
        interrupted: bool,
        bytes: std::io::Cursor<Vec<u8>>,
    }
    impl std::io::Read for Reader {
        fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
            if !self.interrupted {
                self.interrupted = true;
                return Err(std::io::ErrorKind::Interrupted.into());
            }
            self.bytes.read(&mut buffer[..1])
        }
    }
    struct Failed;
    impl std::io::Read for Failed {
        fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
            Err(std::io::ErrorKind::PermissionDenied.into())
        }
    }
    let mut value = candidate();
    value["assets"][1]["bytes"] = json!(3);
    value["assets"][1]["sha256"] = json!(hex(&Sha256::digest(b"abc")));
    let release = authenticated(&value);
    assert_eq!(
        release.verify_package(Reader {
            interrupted: false,
            bytes: std::io::Cursor::new(b"abc".to_vec())
        }),
        Ok(())
    );
    assert_eq!(release.verify_package(Failed), Err("package_read_failed"));
}
