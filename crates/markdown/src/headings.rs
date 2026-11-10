//! Version 1 heading identifiers for a single immutable Markdown snapshot.
//! Parsing uses the same options as preview; offsets address source UTF-8.

use crate::{markdown_options, RenderError, MAX_HTML_BYTES, MAX_SOURCE_BYTES};
use pulldown_cmark::{Event, Parser, Tag, TagEnd};
use std::collections::{HashMap, HashSet};
use std::ops::Range;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HeadingAnchor {
    pub range: Range<usize>,
    pub level: u8,
    pub title: String,
    pub id: String,
}

fn base_id(title: &str) -> String {
    let mut id = String::new();
    let mut separator = false;
    for ch in title.chars().flat_map(char::to_lowercase) {
        if ch.is_alphanumeric() || ch == '_' {
            if separator && !id.is_empty() {
                id.push('-');
            }
            id.push(ch);
            separator = false;
        } else if ch.is_whitespace() || ch == '-' {
            separator = true;
        }
    }
    if id.is_empty() {
        id.push_str("section");
    }
    id
}

/// Extract heading ranges, plain inline text and globally unique IDs.
/// The whole result fails on limits; it is never a silently truncated map.
pub fn analyze(source: &str) -> Result<Vec<HeadingAnchor>, RenderError> {
    if source.len() > MAX_SOURCE_BYTES {
        return Err(RenderError::SourceLimit);
    }
    let mut result = Vec::new();
    let mut current: Option<HeadingAnchor> = None;
    let mut used = HashSet::new();
    let mut next_suffix: HashMap<String, usize> = HashMap::new();
    let mut budget = 0usize;
    for (event, range) in Parser::new_ext(source, markdown_options()).into_offset_iter() {
        match event {
            Event::Start(Tag::Heading { level, .. }) => {
                current = Some(HeadingAnchor {
                    range,
                    level: level as u8,
                    title: String::new(),
                    id: String::new(),
                });
            }
            Event::End(TagEnd::Heading(_)) => {
                if let Some(mut heading) = current.take() {
                    let base = base_id(&heading.title);
                    let mut id = base.clone();
                    if used.contains(&id) {
                        let next = next_suffix.entry(base.clone()).or_insert(1);
                        loop {
                            id = format!("{base}-{next}");
                            *next += 1;
                            if !used.contains(&id) {
                                break;
                            }
                        }
                    }
                    budget = budget
                        .saturating_add(heading.title.len())
                        .saturating_add(id.len())
                        .saturating_add(64);
                    if budget > MAX_HTML_BYTES {
                        return Err(RenderError::OutputLimit);
                    }
                    used.insert(id.clone());
                    heading.id = id;
                    result.push(heading);
                }
            }
            Event::Text(value)
            | Event::Code(value)
            | Event::Html(value)
            | Event::InlineHtml(value) => {
                if let Some(heading) = &mut current {
                    if value.len() > MAX_HTML_BYTES.saturating_sub(heading.title.len()) {
                        return Err(RenderError::OutputLimit);
                    }
                    heading.title.push_str(&value);
                }
            }
            Event::SoftBreak | Event::HardBreak => {
                if let Some(heading) = &mut current {
                    heading.title.push(' ');
                }
            }
            _ => {}
        }
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_cover_unicode_formatting_empty_and_collisions() {
        let source = "# 中文 **加粗** `Code`\n\n# Foo\n# foo-1\n# FOO\n# foo\n#\n# !!!\n\n```\n# ignored\n```\n";
        let headings = analyze(source).unwrap();
        let ids: Vec<_> = headings.iter().map(|item| item.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "中文-加粗-code",
                "foo",
                "foo-1",
                "foo-2",
                "foo-3",
                "section",
                "section-1"
            ]
        );
        assert_eq!(headings[0].title, "中文 加粗 Code");
        for heading in &headings {
            assert!(heading.range.start < heading.range.end);
            assert!(
                source.is_char_boundary(heading.range.start)
                    && source.is_char_boundary(heading.range.end)
            );
            assert!(source[heading.range.clone()].starts_with('#'));
        }
    }

    #[test]
    fn setext_inline_html_and_url_fragments_are_deterministic() {
        let source = "A <b>标签</b> &amp; [link](a.md)\n===\n";
        let headings = analyze(source).unwrap();
        assert_eq!(headings.len(), 1);
        assert_eq!(headings[0].level, 1);
        assert_eq!(headings[0].title, "A <b>标签</b> & link");
        assert_eq!(headings[0].id, "a-b标签b-link");
        let target =
            crate::workspace_path::resolve("doc.md", "#a-b%E6%A0%87%E7%AD%BEb-link").unwrap();
        assert_eq!(target.fragment.as_deref(), Some(headings[0].id.as_str()));
    }

    #[test]
    fn source_limit_is_explicit() {
        assert_eq!(
            analyze(&"x".repeat(MAX_SOURCE_BYTES + 1)),
            Err(RenderError::SourceLimit)
        );
    }
}
