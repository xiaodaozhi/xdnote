//! Locate only destinations inside ranges already admitted by the Markdown
//! parser. A failed or ambiguous location remains read-only, never a substring
//! replacement across source text.
use pulldown_cmark::{Event, LinkType, Parser, Tag};
use std::collections::HashMap;
use std::ops::Range;

fn skip_space(bytes: &[u8], mut at: usize) -> usize {
    while at < bytes.len() && bytes[at].is_ascii_whitespace() {
        at += 1;
    }
    at
}

fn label_end(bytes: &[u8], start: usize) -> Option<usize> {
    let mut at = start;
    let mut depth = 1usize;
    let mut ticks: HashMap<usize, Vec<usize>> = HashMap::new();
    let mut scan = start;
    while scan < bytes.len() {
        if bytes[scan] != b'`' {
            scan += 1;
            continue;
        }
        let run = bytes[scan..].iter().take_while(|b| **b == b'`').count();
        ticks.entry(run).or_default().push(scan);
        scan += run;
    }
    while at < bytes.len() {
        match bytes[at] {
            b'\\' if at + 1 < bytes.len() && bytes[at + 1].is_ascii_punctuation() => at += 2,
            b'`' => {
                let run = bytes[at..].iter().take_while(|b| **b == b'`').count();
                let close = ticks
                    .get(&run)
                    .and_then(|positions| {
                        positions.get(positions.partition_point(|position| *position < at + run))
                    })
                    .map(|position| position + run);
                at = close.unwrap_or(at + run);
            }
            b'[' => {
                depth += 1;
                at += 1;
            }
            b']' => {
                depth -= 1;
                if depth == 0 {
                    return Some(at);
                }
                at += 1;
            }
            _ => at += 1,
        }
    }
    None
}

pub fn destination_range(
    source: &str,
    construct: Range<usize>,
    definition: bool,
    expected: &str,
) -> Option<Range<usize>> {
    let text = source.get(construct.clone())?;
    let bytes = text.as_bytes();
    let mut start = skip_space(bytes, 0);
    if !definition && bytes.get(start) == Some(&b'!') {
        start += 1;
    }
    if bytes.get(start) != Some(&b'[') {
        return None;
    }
    let end = label_end(bytes, start + 1)?;
    if bytes.get(end + 1) != Some(&if definition { b':' } else { b'(' }) {
        return None;
    }
    start = skip_space(bytes, end + 2);
    let angled = bytes.get(start) == Some(&b'<');
    if angled {
        start += 1;
    }
    let mut at = start;
    let mut depth = 0usize;
    while at < bytes.len() {
        let byte = bytes[at];
        if byte == b'\\' && at + 1 < bytes.len() && bytes[at + 1].is_ascii_punctuation() {
            at += 2;
            continue;
        }
        if angled {
            if byte == b'>' {
                break;
            }
            if byte == b'\n' || byte == b'\r' || byte == b'<' {
                return None;
            }
        } else if byte.is_ascii_whitespace() || (byte == b')' && depth == 0) {
            break;
        } else if byte == b'(' {
            depth += 1;
            if depth > 32 {
                return None;
            }
        } else if byte == b')' {
            depth = depth.checked_sub(1)?;
        }
        at += 1;
    }
    if at == start || depth != 0 || (angled && bytes.get(at) != Some(&b'>')) {
        return None;
    }
    let raw = text.get(start..at)?;
    // Re-admit the extracted spelling with the same parser, including entity
    // and backslash decoding. This does not search for the decoded target.
    let probe = format!("[target](<{raw}>)");
    let admitted = Parser::new_ext(&probe, super::markdown_options())
        .into_offset_iter()
        .find_map(|(event, range)| match event {
            Event::Start(Tag::Link {
                link_type: LinkType::Inline,
                dest_url,
                ..
            }) if range == (0..probe.len()) => Some(dest_url.as_ref() == expected),
            _ => None,
        });
    if admitted != Some(true) {
        return None;
    }
    Some(construct.start + start..construct.start + at)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn admitted_ranges_preserve_label_title_entities_and_nested_syntax() {
        for (source, target, raw) in [
            (
                "[a `]` **b**](folder/a\\(b\\).md \"title\")",
                "folder/a(b).md",
                "folder/a\\(b\\).md",
            ),
            ("![图](<图%20片.png> 'title')", "图%20片.png", "图%20片.png"),
            ("[![alt](image.png)](other.md)", "other.md", "other.md"),
            ("[a](a&amp;b.md)", "a&b.md", "a&amp;b.md"),
        ] {
            let range = destination_range(source, 0..source.len(), false, target).unwrap();
            assert_eq!(&source[range], raw);
        }
        let source = "[shared]:\n  <../notes/中文.md#标题> \"title\"\n";
        let range =
            destination_range(source, 0..source.len(), true, "../notes/中文.md#标题").unwrap();
        assert_eq!(&source[range], "../notes/中文.md#标题");
        assert!(destination_range("[x](actual.md)", 0..14, false, "different.md").is_none());
        assert!(destination_range("literal actual.md", 0..17, false, "actual.md").is_none());
    }
}
