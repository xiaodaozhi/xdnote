//! A parser-only experiment, not the xdnote renderer or a security filter.

use pulldown_cmark::Options;

pub fn options() -> Options {
    Options::ENABLE_TABLES | Options::ENABLE_TASKLISTS | Options::ENABLE_STRIKETHROUGH
}

#[cfg(test)]
mod tests {
    use super::options;
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
}
