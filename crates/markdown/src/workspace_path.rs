//! Lexical Markdown path handling. This does not open files or grant access.
//! Consumers must separately validate the actual opened filesystem objects.

const MAX_PATH_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PathError {
    InvalidDocument,
    EmptyTarget,
    AbsoluteOrExternal,
    QueryNotSupported,
    InvalidEscape,
    InvalidUtf8,
    InvalidCharacter,
    EscapesWorkspace,
    TooLong,
}

impl PathError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::InvalidDocument => "invalid-document",
            Self::EmptyTarget => "empty-target",
            Self::AbsoluteOrExternal => "absolute-or-external",
            Self::QueryNotSupported => "query-not-supported",
            Self::InvalidEscape => "invalid-escape",
            Self::InvalidUtf8 => "invalid-utf8",
            Self::InvalidCharacter => "invalid-character",
            Self::EscapesWorkspace => "escapes-workspace",
            Self::TooLong => "too-long",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelativeTarget {
    /// Root-relative path, not an authorization or a canonical filesystem path.
    pub path: String,
    /// A decoded fragment; `Some("")` remains distinct from no fragment.
    pub fragment: Option<String>,
}

fn input_path(path: &str) -> Result<Vec<&str>, PathError> {
    if path.len() > MAX_PATH_BYTES {
        return Err(PathError::TooLong);
    }
    if path.is_empty() || path.starts_with('/') || path.contains('\\') || path.contains('\0') {
        return Err(PathError::InvalidDocument);
    }
    let parts: Vec<_> = path.split('/').collect();
    if parts.iter().any(|part| {
        part.is_empty() || *part == "." || *part == ".." || part.chars().any(char::is_control)
    }) {
        return Err(PathError::InvalidDocument);
    }
    Ok(parts)
}

fn hex(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn decode(component: &str, path_component: bool) -> Result<String, PathError> {
    let source = component.as_bytes();
    let mut bytes = Vec::with_capacity(source.len());
    let mut position = 0;
    while position < source.len() {
        let byte = if source[position] == b'%' {
            let high = source
                .get(position + 1)
                .and_then(|byte| hex(*byte))
                .ok_or(PathError::InvalidEscape)?;
            let low = source
                .get(position + 2)
                .and_then(|byte| hex(*byte))
                .ok_or(PathError::InvalidEscape)?;
            position += 3;
            (high << 4) | low
        } else {
            let value = source[position];
            position += 1;
            value
        };
        if byte < 0x20 || byte == 0x7f || (path_component && (byte == b'/' || byte == b'\\')) {
            return Err(PathError::InvalidCharacter);
        }
        bytes.push(byte);
    }
    String::from_utf8(bytes).map_err(|_| PathError::InvalidUtf8)
}

fn has_scheme(path: &str) -> bool {
    let first = path.split('/').next().unwrap_or("");
    let Some((prefix, _)) = first.split_once(':') else {
        return false;
    };
    let mut chars = prefix.bytes();
    matches!(chars.next(), Some(b'a'..=b'z' | b'A'..=b'Z'))
        && chars.all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'+' | b'-' | b'.'))
}

/// Parse a Markdown target relative to the document directory. Percent escapes
/// are decoded once per component. A normalized path is not proof that an open
/// object is inside the workspace: links and namespace races remain separate.
pub fn resolve(document: &str, target: &str) -> Result<RelativeTarget, PathError> {
    let document_parts = input_path(document)?;
    if target.len() > MAX_PATH_BYTES {
        return Err(PathError::TooLong);
    }
    let (path, fragment) = match target.split_once('#') {
        Some((path, value)) => (path, Some(decode(value, false)?)),
        None => (target, None),
    };
    if path.contains('?') {
        return Err(PathError::QueryNotSupported);
    }
    if path.starts_with('/') || path.contains('\\') || has_scheme(path) {
        return Err(PathError::AbsoluteOrExternal);
    }
    if path.is_empty() {
        return if fragment.is_some() {
            Ok(RelativeTarget {
                path: document.to_owned(),
                fragment,
            })
        } else {
            Err(PathError::EmptyTarget)
        };
    }
    let mut parts: Vec<String> = document_parts[..document_parts.len() - 1]
        .iter()
        .map(|part| (*part).to_owned())
        .collect();
    for raw in path.split('/') {
        if raw.is_empty() {
            return Err(PathError::InvalidCharacter);
        }
        let part = decode(raw, true)?;
        match part.as_str() {
            "." => (),
            ".." => {
                if parts.pop().is_none() {
                    return Err(PathError::EscapesWorkspace);
                }
            }
            _ => parts.push(part),
        }
    }
    if parts.is_empty() {
        return Err(PathError::EmptyTarget);
    }
    Ok(RelativeTarget {
        path: parts.join("/"),
        fragment,
    })
}

fn encode_component(value: &str, output: &mut String) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    for byte in value.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
            output.push(char::from(byte));
        } else {
            output.push('%');
            output.push(char::from(HEX[(byte >> 4) as usize]));
            output.push(char::from(HEX[(byte & 15) as usize]));
        }
    }
}

/// Generate a standard relative URL from two already root-relative paths.
pub fn relative_url(document: &str, destination: &str) -> Result<String, PathError> {
    let from = input_path(document)?;
    let to = input_path(destination)?;
    let parent = &from[..from.len() - 1];
    let mut common = parent
        .iter()
        .zip(&to)
        .take_while(|(left, right)| left == right)
        .count();
    // A destination equal to an ancestor still needs a final path component;
    // an empty URL would refer to the current document instead.
    if common == to.len() && common > 0 {
        common -= 1;
    }
    let mut output = String::new();
    for _ in common..parent.len() {
        output.push_str("../");
    }
    for (index, component) in to[common..].iter().enumerate() {
        if index != 0 {
            output.push('/');
        }
        encode_component(component, &mut output);
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relative_paths_round_trip_unicode_and_reserved_characters() {
        for (document, destination) in [
            ("README.md", "assets/images/示意图 #1%?.png"),
            ("notes/子目录/方案.md", "assets/images/猫+狗(1).png"),
            ("notes/方案.md", "notes/另一个.md"),
            ("a/b/c.md", "a"),
        ] {
            let url = relative_url(document, destination).unwrap();
            assert!(!url.contains(' ') && !url.contains('#') && !url.contains('?'));
            assert_eq!(
                resolve(document, &url).unwrap(),
                RelativeTarget {
                    path: destination.to_owned(),
                    fragment: None
                }
            );
        }
        assert_eq!(
            resolve("notes/一.md", "../assets/a%23b%25.png#%E6%A0%87%E9%A2%98").unwrap(),
            RelativeTarget {
                path: "assets/a#b%.png".into(),
                fragment: Some("标题".into())
            }
        );
        assert_eq!(
            resolve("notes/一.md", "#part+one")
                .unwrap()
                .fragment
                .as_deref(),
            Some("part+one")
        );
        assert_eq!(
            resolve("notes/一.md", "x%252Fy.png").unwrap().path,
            "notes/x%2Fy.png"
        );
    }

    #[test]
    fn rejects_escape_unsafe_or_ambiguous_targets() {
        let doc = "notes/one.md";
        for path in ["../../secret", "%2e%2e/%2e%2e/secret"] {
            assert_eq!(resolve(doc, path), Err(PathError::EscapesWorkspace));
        }
        for path in [
            "/etc/passwd",
            "//server/share",
            "file:///x",
            "https://x",
            "C:/x",
            "x\\y",
        ] {
            assert_eq!(resolve(doc, path), Err(PathError::AbsoluteOrExternal));
        }
        for path in ["x%2Fy", "x%5cy", "x%00y", "x//y"] {
            assert_eq!(resolve(doc, path), Err(PathError::InvalidCharacter));
        }
        for path in ["x%", "x%2", "x%GG"] {
            assert_eq!(resolve(doc, path), Err(PathError::InvalidEscape));
        }
        assert_eq!(resolve(doc, "x%FF"), Err(PathError::InvalidUtf8));
        assert_eq!(resolve(doc, "x?query"), Err(PathError::QueryNotSupported));
        assert_eq!(
            resolve("../outside.md", "x"),
            Err(PathError::InvalidDocument)
        );
        assert_eq!(resolve(doc, ""), Err(PathError::EmptyTarget));
    }
}
