//! A parser-only experiment, not the xdnote renderer or a security filter.

use pulldown_cmark::Options;
use std::ops::Range;

/// A fixture limit for the worker protocol, not an xdnote product limit.
pub const PROBE_MAX_SOURCE_BYTES: usize = 1_048_576;

/// Tracks the one generation whose result the isolated probe may still accept.
#[derive(Default)]
pub struct GenerationGate {
    issued: u64,
    current: Option<u64>,
}

impl GenerationGate {
    pub fn begin(&mut self) -> Option<u64> {
        let generation = self.issued.checked_add(1)?;
        self.issued = generation;
        self.current = Some(generation);
        Some(generation)
    }

    pub fn cancel(&mut self) {
        self.current = None;
    }

    pub fn accept(&mut self, generation: u64) -> bool {
        if self.current != Some(generation) {
            return false;
        }
        self.current = None;
        true
    }
}

pub fn options() -> Options {
    Options::ENABLE_TABLES | Options::ENABLE_TASKLISTS | Options::ENABLE_STRIKETHROUGH
}

/// Map an offset in the unchanged UTF-8 source to UTF-16 code units.
/// This linear reference implementation is not a production indexing strategy.
pub fn byte_offset_to_utf16(source: &str, offset: usize) -> Option<usize> {
    if offset > source.len() || !source.is_char_boundary(offset) {
        return None;
    }
    Some(source[..offset].encode_utf16().count())
}

/// Map UTF-16 code units back to a UTF-8 byte boundary in the same source.
/// An offset between the two halves of a supplementary character is invalid.
pub fn utf16_offset_to_byte(source: &str, offset: usize) -> Option<usize> {
    let mut units = 0;
    for (byte, character) in source.char_indices() {
        if units == offset {
            return Some(byte);
        }
        units += character.len_utf16();
        if units > offset {
            return None;
        }
    }
    (units == offset).then_some(source.len())
}

pub fn byte_range_to_utf16(source: &str, range: Range<usize>) -> Option<Range<usize>> {
    if range.start > range.end {
        return None;
    }
    Some(byte_offset_to_utf16(source, range.start)?..byte_offset_to_utf16(source, range.end)?)
}

#[cfg(test)]
mod tests {
    use super::{byte_offset_to_utf16, byte_range_to_utf16, options, utf16_offset_to_byte};
    use pulldown_cmark::{CodeBlockKind, Event, HeadingLevel, Parser, Tag};

    const CORPUS: &str = include_str!("../../qt-document-spike/tests/fixtures/markdown-core.md");

    #[test]
    fn shared_corpus_has_expected_structural_events() {
        let events: Vec<_> = Parser::new_ext(CORPUS, options()).collect();
        assert!(events.iter().any(|event| matches!(
            event,
            Event::Start(Tag::Heading {
                level: HeadingLevel::H1,
                ..
            })
        )));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::Start(Tag::Strong))));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::Start(Tag::Emphasis))));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::Start(Tag::BlockQuote(_)))));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::Start(Tag::List(Some(1))))));
        assert!(events.iter().any(|event| matches!(event,
            Event::Start(Tag::CodeBlock(CodeBlockKind::Fenced(language))) if language.as_ref() == "cpp")));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::Start(Tag::Table(_)))));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::TaskListMarker(true))));
        assert!(events
            .iter()
            .any(|event| matches!(event, Event::TaskListMarker(false))));
        assert!(Parser::new_ext("~~划掉~~", options())
            .any(|event| matches!(event, Event::Start(Tag::Strikethrough))));
        for fragment in [
            "标题",
            "加粗",
            "斜体",
            "引用",
            "第一",
            "int x = 1;",
            "甲",
            "乙",
        ] {
            assert!(
                events.iter().any(|event| matches!(event,
                Event::Text(text) if text.contains(fragment))),
                "missing text {fragment}"
            );
        }
    }

    #[test]
    fn markup_and_destinations_are_untrusted_events() {
        let events: Vec<_> = Parser::new_ext(CORPUS, options()).collect();
        assert!(events.iter().any(|event| matches!(event,
            Event::Start(Tag::Link { dest_url, .. }) if dest_url.as_ref() == "https://example.invalid/a")));
        assert!(events.iter().any(|event| matches!(event,
            Event::Start(Tag::Link { dest_url, .. }) if dest_url.as_ref() == "javascript:alert%281%29")));
        assert!(events.iter().any(|event| matches!(event,
            Event::Html(html) if html.contains("<script>alert(1)</script>"))));

        let images =
            "![local](assets/a.png) ![parent](../a.png) ![remote](https://example.invalid/a.png)";
        let destinations: Vec<_> = Parser::new_ext(images, options())
            .filter_map(|event| match event {
                Event::Start(Tag::Image { dest_url, .. }) => Some(dest_url.into_string()),
                _ => None,
            })
            .collect();
        assert_eq!(
            destinations,
            ["assets/a.png", "../a.png", "https://example.invalid/a.png"]
        );
    }

    #[test]
    fn unicode_offsets_are_utf8_byte_ranges_in_the_original_source() {
        let source = "# 甲🙂\n\n正文 **乙**\n";
        let mut heading_text = None;
        let mut strong_text = None;
        for (event, range) in Parser::new_ext(source, options()).into_offset_iter() {
            assert!(range.start <= range.end && range.end <= source.len());
            assert!(source.is_char_boundary(range.start) && source.is_char_boundary(range.end));
            if let Event::Text(text) = event {
                if text.as_ref() == "甲🙂" {
                    heading_text = Some(range);
                } else if text.as_ref() == "乙" {
                    strong_text = Some(range);
                }
            }
        }
        let heading = heading_text.expect("heading text event");
        let strong = strong_text.expect("strong text event");
        assert_eq!(&source[heading.clone()], "甲🙂");
        assert_eq!(&source[strong], "乙");
        assert_eq!(heading.end - heading.start, "甲🙂".len());
        assert_ne!("甲🙂".len(), "甲🙂".encode_utf16().count());
    }

    #[test]
    fn offset_mapping_respects_unicode_and_crlf_boundaries() {
        let source = "甲🙂e\u{301}\r\n乙";
        // Combining marks and both CRLF code units remain distinct source positions.
        let boundaries = [
            (0, 0),
            (3, 1),
            (7, 3),
            (8, 4),
            (10, 5),
            (11, 6),
            (12, 7),
            (15, 8),
        ];
        for (byte, utf16) in boundaries {
            assert_eq!(byte_offset_to_utf16(source, byte), Some(utf16));
            assert_eq!(utf16_offset_to_byte(source, utf16), Some(byte));
        }
        for byte in [1, 2, 4, 5, 6, 9, 13, 14, 16] {
            assert_eq!(byte_offset_to_utf16(source, byte), None);
        }
        assert_eq!(utf16_offset_to_byte(source, 2), None); // Inside the emoji's surrogate pair.
        assert_eq!(utf16_offset_to_byte(source, 9), None);
        assert_eq!(byte_range_to_utf16(source, 3..7), Some(1..3));
        assert_eq!(byte_range_to_utf16(source, 4..7), None);
        assert_eq!(byte_range_to_utf16(source, 7..3), None);
        assert_eq!(byte_range_to_utf16(source, 0..16), None);
        assert_eq!(byte_offset_to_utf16("", 0), Some(0));
        assert_eq!(utf16_offset_to_byte("", 0), Some(0));
    }

    #[test]
    fn parser_ranges_map_to_source_not_decoded_rendered_text() {
        let source = "# 甲🙂\r\n\r\n文 &amp; 🙂\n";
        let mut decoded_entity_range = None;
        for input in [source, CORPUS] {
            for (event, bytes) in Parser::new_ext(input, options()).into_offset_iter() {
                let utf16 = byte_range_to_utf16(input, bytes.clone()).expect("valid source range");
                assert_eq!(utf16_offset_to_byte(input, utf16.start), Some(bytes.start));
                assert_eq!(utf16_offset_to_byte(input, utf16.end), Some(bytes.end));
                if input == source {
                    if let Event::Text(text) = event {
                        if text.as_ref() == "&" {
                            decoded_entity_range = Some(bytes);
                        }
                    }
                }
            }
        }
        let entity = decoded_entity_range.expect("decoded entity event");
        assert_eq!(&source[entity], "&amp;");
    }
}
