//! Synchronous bounded JSON C ABI. Rust owns every returned allocation.
use super::*;
use serde_json::json;

const REQUEST_LIMIT: usize = 2 * 1024 * 1024;
#[derive(Deserialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
enum Request {
    Compare {
        left: String,
        right: String,
    },
    Eligible {
        tag: String,
        current: String,
        channel: String,
    },
    Inspect {
        manifest_hex: String,
        current: String,
        current_build: u32,
        current_channel: String,
        platform: String,
        architecture: String,
        package_type: String,
    },
    Verify {
        manifest_hex: String,
        signature_hex: String,
    },
}

fn raw_bytes(value: &str, limit: usize) -> Result<Vec<u8>, ()> {
    if value.len() > limit * 2
        || !value.len().is_multiple_of(2)
        || !lower_hex(value, value.len() / 2)
    {
        return Err(());
    }
    (0..value.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&value[i..i + 2], 16).map_err(|_| ()))
        .collect()
}

fn dispatch(bytes: &[u8]) -> Result<Vec<u8>, ()> {
    let request: Request = serde_json::from_slice(bytes).map_err(|_| ())?;
    let response = match request {
        Request::Compare { left, right } => json!({"comparison": compare_versions(&left, &right)}),
        Request::Eligible {
            tag,
            current,
            channel,
        } => json!({"eligible": release_tag_eligible(&tag, &current, &channel)}),
        Request::Inspect {
            manifest_hex,
            current,
            current_build,
            current_channel,
            platform,
            architecture,
            package_type,
        } => {
            let bytes = raw_bytes(&manifest_hex, MANIFEST_LIMIT)?;
            let manifest = std::str::from_utf8(&bytes).map_err(|_| ())?;
            json!(inspect_release(
                manifest,
                &current,
                current_build,
                &current_channel,
                &platform,
                &architecture,
                &package_type
            ))
        }
        Request::Verify {
            manifest_hex,
            signature_hex,
        } => {
            let manifest = raw_bytes(&manifest_hex, MANIFEST_LIMIT)?;
            let signature = raw_bytes(&signature_hex, SIGNATURE_LIMIT)?;
            let signature = std::str::from_utf8(&signature).map_err(|_| ())?;
            json!({"error": verify_manifest(&manifest, signature, PRODUCTION_KEYRING)})
        }
    };
    serde_json::to_vec(&response).map_err(|_| ())
}

#[repr(C)]
pub struct UpdateBuffer {
    data: *mut u8,
    length: usize,
    status: u32,
}
impl UpdateBuffer {
    fn error(status: u32) -> Self {
        Self {
            data: std::ptr::null_mut(),
            length: 0,
            status,
        }
    }
}

/// # Safety
/// Non-null data must address length readable bytes for this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn xdnote_update_request(data: *const u8, length: usize) -> UpdateBuffer {
    if data.is_null() || length > REQUEST_LIMIT {
        return UpdateBuffer::error(1);
    }
    let result = std::panic::catch_unwind(|| {
        // SAFETY: caller guarantees the borrowed input range.
        let bytes = unsafe { std::slice::from_raw_parts(data, length) };
        dispatch(bytes)
    });
    match result {
        Ok(Ok(bytes)) => {
            let mut bytes = bytes.into_boxed_slice();
            let result = UpdateBuffer {
                data: bytes.as_mut_ptr(),
                length: bytes.len(),
                status: 0,
            };
            std::mem::forget(bytes);
            result
        }
        Ok(Err(())) => UpdateBuffer::error(2),
        Err(_) => UpdateBuffer::error(3),
    }
}

/// # Safety
/// buffer must be null or a live buffer returned by xdnote_update_request,
/// released exactly once. The same cleared buffer may be released again.
#[no_mangle]
pub unsafe extern "C" fn xdnote_update_free(buffer: *mut UpdateBuffer) {
    // SAFETY: the caller provides a live output structure, or null.
    if let Some(buffer) = unsafe { buffer.as_mut() } {
        if !buffer.data.is_null() {
            // SAFETY: this exact pointer/length came from Box<[u8]> above.
            unsafe {
                drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                    buffer.data,
                    buffer.length,
                )))
            };
        }
        *buffer = UpdateBuffer::error(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn raw_byte_transport_preserves_bom_nul_and_rejects_invalid_hex() {
        assert_eq!(
            raw_bytes("efbbbf00ff", 5).unwrap(),
            [0xef, 0xbb, 0xbf, 0, 0xff]
        );
        for value in ["0", "FF", "zz", "000000"] {
            assert!(raw_bytes(value, 2).is_err());
        }
        let result = dispatch(br#"{"operation":"inspect","manifest_hex":"efbbbf7b7d","current":"0.1.0-dev.123456abcdef","current_build":0,"current_channel":"development","platform":"windows","architecture":"x86_64","package_type":"zip"}"#).unwrap();
        let result: serde_json::Value = serde_json::from_slice(&result).unwrap();
        assert_eq!(result["error"], "invalid_manifest_json");
        assert_eq!(result["valid"], false);
    }
    #[test]
    fn abi_bounds_ownership_and_runtime_trust_override() {
        for (data, length) in [(std::ptr::null(), 0), (b"x".as_ptr(), REQUEST_LIMIT + 1)] {
            // SAFETY: invalid lengths are rejected before input is read.
            assert_ne!(unsafe { xdnote_update_request(data, length) }.status, 0);
        }
        for bytes in [
            b"{bad".as_slice(),
            &[0xff],
            br#"{"operation":"verify","manifest_hex":"","signature_hex":"","keyring":"injected"}"#,
        ] {
            // SAFETY: the complete input slice remains live through the call.
            let mut output = unsafe { xdnote_update_request(bytes.as_ptr(), bytes.len()) };
            assert_ne!(output.status, 0);
            // SAFETY: one live output; repeated free is allowed after clearing.
            unsafe {
                xdnote_update_free(&mut output);
                xdnote_update_free(&mut output);
            }
        }
        let bytes = br#"{"operation":"compare","left":"0.1.0-alpha.9","right":"0.1.0-alpha.10"}"#;
        // SAFETY: live input and unique returned allocation.
        let mut output = unsafe { xdnote_update_request(bytes.as_ptr(), bytes.len()) };
        assert_eq!(output.status, 0);
        // SAFETY: live Rust-owned output is readable until free.
        assert_eq!(
            unsafe { std::slice::from_raw_parts(output.data, output.length) },
            br#"{"comparison":-1}"#
        );
        // SAFETY: release the returned allocation.
        unsafe {
            xdnote_update_free(&mut output);
        }
        assert!(output.data.is_null());
    }
}
