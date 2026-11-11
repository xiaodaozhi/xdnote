//! One global parse, flat preorder block/inline tree, no HTML or layout.
//! Wire contract: docs/engineering/native-preview-model.md. All source ranges
//! address the caller's immutable UTF-8 snapshot; derived strings are not edits.

use crate::{headings::HeadingIds, markdown_options, reference_expansion_complete};
use crate::{RenderError, MAX_SOURCE_BYTES};
use pulldown_cmark::{Alignment, CodeBlockKind, Event, Parser, Tag, TagEnd};
use std::ops::Range;

pub const MAGIC: &[u8; 8] = b"XDNOTE\0M";
pub const VERSION: u32 = 1;
pub const HEADER_BYTES: usize = 32;
/// A derived-output guard, not a new Markdown file-size limit. Counts the exact
/// returned bytes; it is not a bound on allocator capacity or parser memory.
pub const MAX_MODEL_BYTES: usize = 64 * 1024 * 1024;
pub const NO_PARENT: u32 = u32::MAX;

fn varint(bytes: &mut Vec<u8>, mut value: u32) {
    while value >= 128 {
        bytes.push((value as u8 & 127) | 128);
        value >>= 7;
    }
    bytes.push(value as u8);
}

/// Values are part of the versioned C ABI. Containers end at `subtree_end`;
/// leaves have `subtree_end == index + 1`. Never expose pulldown enum ordinals.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Paragraph = 1,
    Heading = 2,
    BlockQuote = 3,
    CodeBlock = 4,
    LiteralBlock = 5,
    BulletList = 6,
    OrderedList = 7,
    Item = 8,
    Table = 9,
    TableHead = 10,
    TableRow = 11,
    TableCell = 12,
    Emphasis = 13,
    Strong = 14,
    Strikethrough = 15,
    Link = 16,
    Image = 17,
    Text = 18,
    InlineCode = 19,
    LiteralHtml = 20,
    SoftBreak = 21,
    HardBreak = 22,
    Rule = 23,
    TaskMarker = 24,
}

struct Builder {
    records: Vec<u8>,
    strings: Vec<u8>,
    offsets: Vec<u32>,
    stack: Vec<(u32, TagEnd)>,
    count: u32,
    heading: Option<(u32, String, usize)>,
    ids: HeadingIds,
    links: u32,
    images: u32,
    codes: u32,
    limit: usize,
}

impl Builder {
    fn new(limit: usize) -> Self {
        Self {
            records: vec![0; HEADER_BYTES],
            strings: Vec::new(),
            offsets: Vec::new(),
            stack: Vec::new(),
            count: 0,
            heading: None,
            ids: HeadingIds::default(),
            links: 0,
            images: 0,
            codes: 0,
            limit,
        }
    }

    fn reserve(&self, extra: usize) -> Result<(), RenderError> {
        if self
            .records
            .len()
            .saturating_add(self.strings.len())
            .saturating_add(self.offsets.len().saturating_mul(4))
            .saturating_add(extra)
            > self.limit
        {
            return Err(RenderError::OutputLimit);
        }
        Ok(())
    }

    fn string(&mut self, value: &str) -> Result<(u32, u32), RenderError> {
        self.reserve(value.len())?;
        let offset = self.strings.len() as u32;
        self.strings.extend_from_slice(value.as_bytes());
        Ok((offset, value.len() as u32))
    }

    fn node(
        &mut self,
        kind: Kind,
        range: Range<usize>,
        text: &str,
        auxiliary: &str,
        detail: u32,
    ) -> Result<u32, RenderError> {
        let text = self.string(text)?;
        let auxiliary = self.string(auxiliary)?;
        let index = self.count;
        self.count += 1;
        self.offsets
            .push((self.records.len() - HEADER_BYTES) as u32);
        let has_text = text.1 != 0 || kind == Kind::Heading;
        let has_auxiliary = auxiliary.1 != 0;
        self.records.push(kind as u8);
        self.records.push(
            u8::from(has_text) | (u8::from(has_auxiliary) << 1) | (u8::from(detail != 0) << 2),
        );
        self.records.extend_from_slice(&self.count.to_le_bytes());
        varint(
            &mut self.records,
            self.stack.last().map_or(0, |entry| index - entry.0),
        );
        varint(&mut self.records, range.start as u32);
        varint(&mut self.records, (range.end - range.start) as u32);
        if has_text {
            if kind == Kind::Heading {
                // Heading IDs become known at End(Heading); fixed fields can
                // be patched without shifting any previously indexed record.
                self.heading = Some((index, String::new(), self.records.len()));
                self.records.extend_from_slice(&text.0.to_le_bytes());
                self.records.extend_from_slice(&text.1.to_le_bytes());
            } else {
                varint(&mut self.records, text.0);
                varint(&mut self.records, text.1);
            }
        }
        if has_auxiliary {
            varint(&mut self.records, auxiliary.0);
            varint(&mut self.records, auxiliary.1);
        }
        if detail != 0 {
            varint(&mut self.records, detail);
        }
        self.reserve(0)?;
        Ok(index)
    }

    fn start(&mut self, tag: Tag<'_>, range: Range<usize>) -> Result<(), RenderError> {
        let end = tag.to_end();
        let index = match tag {
            Tag::Paragraph => self.node(Kind::Paragraph, range, "", "", 0)?,
            Tag::Heading { level, .. } => self.node(Kind::Heading, range, "", "", level as u32)?,
            Tag::BlockQuote(_) => self.node(Kind::BlockQuote, range, "", "", 0)?,
            Tag::CodeBlock(kind) => {
                let language = match &kind {
                    CodeBlockKind::Fenced(info) => info.split_whitespace().next().unwrap_or(""),
                    CodeBlockKind::Indented => "",
                };
                let index = self.node(Kind::CodeBlock, range, language, "", self.codes)?;
                self.codes += 1;
                index
            }
            Tag::HtmlBlock => self.node(Kind::LiteralBlock, range, "", "", 0)?,
            Tag::List(start) => self.node(
                if start.is_some() {
                    Kind::OrderedList
                } else {
                    Kind::BulletList
                },
                range,
                "",
                "",
                start.unwrap_or(0) as u32,
            )?,
            Tag::Item => self.node(Kind::Item, range, "", "", 0)?,
            Tag::Table(columns) => {
                let alignments: String = columns
                    .iter()
                    .map(|alignment| match alignment {
                        Alignment::None => 'N',
                        Alignment::Left => 'L',
                        Alignment::Center => 'C',
                        Alignment::Right => 'R',
                    })
                    .collect();
                self.node(Kind::Table, range, &alignments, "", columns.len() as u32)?
            }
            Tag::TableHead => self.node(Kind::TableHead, range, "", "", 0)?,
            Tag::TableRow => self.node(Kind::TableRow, range, "", "", 0)?,
            Tag::TableCell => self.node(Kind::TableCell, range, "", "", 0)?,
            Tag::Emphasis => self.node(Kind::Emphasis, range, "", "", 0)?,
            Tag::Strong => self.node(Kind::Strong, range, "", "", 0)?,
            Tag::Strikethrough => self.node(Kind::Strikethrough, range, "", "", 0)?,
            Tag::Link {
                dest_url, title, ..
            } => {
                let index = self.node(Kind::Link, range, &dest_url, &title, self.links)?;
                self.links += 1;
                index
            }
            Tag::Image {
                dest_url, title, ..
            } => {
                let index = self.node(Kind::Image, range, &dest_url, &title, self.images)?;
                self.images += 1;
                index
            }
            // These extensions are not enabled. Fail closed if the dialect is
            // later changed without updating this model's versioned contract.
            _ => return Err(RenderError::OutputLimit),
        };
        self.stack.push((index, end));
        Ok(())
    }

    fn end(&mut self, tag: TagEnd) -> Result<(), RenderError> {
        let (index, expected) = self.stack.pop().ok_or(RenderError::OutputLimit)?;
        if tag != expected {
            return Err(RenderError::OutputLimit);
        }
        let at = HEADER_BYTES + self.offsets[index as usize] as usize + 2;
        self.records[at..at + 4].copy_from_slice(&self.count.to_le_bytes());
        if matches!(tag, TagEnd::Heading(_)) {
            let (heading_index, title, patch) =
                self.heading.take().ok_or(RenderError::OutputLimit)?;
            if heading_index != index {
                return Err(RenderError::OutputLimit);
            }
            let id = self.ids.next(&title);
            let (offset, length) = self.string(&id)?;
            self.records[patch..patch + 4].copy_from_slice(&offset.to_le_bytes());
            self.records[patch + 4..patch + 8].copy_from_slice(&length.to_le_bytes());
        }
        Ok(())
    }

    fn leaf(&mut self, event: Event<'_>, range: Range<usize>) -> Result<(), RenderError> {
        let (kind, text, detail) = match event {
            Event::Text(value) => (Kind::Text, value, 0),
            Event::Code(value) => (Kind::InlineCode, value, 0),
            Event::Html(value) | Event::InlineHtml(value) => (Kind::LiteralHtml, value, 0),
            Event::SoftBreak => (Kind::SoftBreak, "".into(), 0),
            Event::HardBreak => (Kind::HardBreak, "".into(), 0),
            Event::Rule => (Kind::Rule, "".into(), 0),
            Event::TaskListMarker(checked) => (Kind::TaskMarker, "".into(), u32::from(checked)),
            _ => return Err(RenderError::OutputLimit),
        };
        self.node(kind, range, &text, "", detail)?;
        if let Some((_, title, _)) = &mut self.heading {
            title.push_str(if matches!(kind, Kind::SoftBreak | Kind::HardBreak) {
                " "
            } else {
                &text
            });
        }
        Ok(())
    }

    fn finish(
        mut self,
        source_bytes: usize,
        references_complete: bool,
    ) -> Result<Vec<u8>, RenderError> {
        if !self.stack.is_empty() || self.heading.is_some() {
            return Err(RenderError::OutputLimit);
        }
        self.reserve(0)?;
        self.records[..8].copy_from_slice(MAGIC);
        for (index, value) in [
            VERSION,
            source_bytes as u32,
            self.count,
            self.strings.len() as u32,
            u32::from(references_complete),
            (self.records.len() - HEADER_BYTES) as u32,
        ]
        .iter()
        .enumerate()
        {
            self.records[8 + index * 4..12 + index * 4].copy_from_slice(&value.to_le_bytes());
        }
        self.records
            .reserve_exact(self.offsets.len() * 4 + self.strings.len());
        for offset in self.offsets {
            self.records.extend_from_slice(&offset.to_le_bytes());
        }
        self.records.extend_from_slice(&self.strings);
        Ok(self.records)
    }
}

/// Unlike the retired H1 fixture partitioner, this includes all block and
/// inline structure, including tight-list inline children and single large
/// blocks. It does not perform layout or split source into separately parsed
/// fragments. Reference definitions are resolved by this same parser instance.
pub fn build(source: &str) -> Result<Vec<u8>, RenderError> {
    build_with_limit(source, MAX_MODEL_BYTES)
}

fn build_with_limit(source: &str, limit: usize) -> Result<Vec<u8>, RenderError> {
    if source.len() > MAX_SOURCE_BYTES {
        return Err(RenderError::SourceLimit);
    }
    let events = Parser::new_ext(source, markdown_options()).into_offset_iter();
    let largest_reference = events
        .reference_definitions()
        .iter()
        .map(|(_, definition)| {
            definition
                .dest
                .len()
                .saturating_add(definition.title.as_ref().map_or(0, |title| title.len()))
        })
        .max()
        .unwrap_or(0);
    let complete = reference_expansion_complete(source, largest_reference);
    let mut model = Builder::new(limit);
    for (event, range) in events {
        match event {
            Event::Start(tag) => model.start(tag, range)?,
            Event::End(tag) => model.end(tag)?,
            event => model.leaf(event, range)?,
        }
    }
    model.finish(source.len(), complete)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn word(bytes: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }
    fn field(bytes: &[u8], index: u32, field: usize) -> u32 {
        fn read(bytes: &[u8], at: &mut usize) -> u32 {
            let mut value = 0;
            let mut shift = 0;
            loop {
                let byte = bytes[*at];
                *at += 1;
                value |= u32::from(byte & 127) << shift;
                if byte < 128 {
                    return value;
                }
                shift += 7;
            }
        }
        let offsets = HEADER_BYTES + word(bytes, 28) as usize;
        let at = HEADER_BYTES + word(bytes, offsets + index as usize * 4) as usize;
        let kind = u32::from(bytes[at]);
        let flags = bytes[at + 1];
        let mut next = at + 6;
        let delta = read(bytes, &mut next);
        let parent = if delta == 0 { NO_PARENT } else { index - delta };
        let start = read(bytes, &mut next);
        let length = read(bytes, &mut next);
        let mut text = (0, 0);
        let mut auxiliary = (0, 0);
        if flags & 1 != 0 {
            text = if kind == Kind::Heading as u32 {
                let value = (word(bytes, next), word(bytes, next + 4));
                next += 8;
                value
            } else {
                (read(bytes, &mut next), read(bytes, &mut next))
            };
        }
        if flags & 2 != 0 {
            auxiliary = (read(bytes, &mut next), read(bytes, &mut next));
        }
        let detail = if flags & 4 != 0 {
            read(bytes, &mut next)
        } else {
            0
        };
        [
            kind,
            parent,
            word(bytes, at + 2),
            start,
            start + length,
            text.0,
            text.1,
            auxiliary.0,
            auxiliary.1,
            detail,
        ][field]
    }
    fn text(bytes: &[u8], index: u32) -> &str {
        let arena = HEADER_BYTES + word(bytes, 28) as usize + word(bytes, 16) as usize * 4;
        let start = arena + field(bytes, index, 5) as usize;
        std::str::from_utf8(&bytes[start..start + field(bytes, index, 6) as usize]).unwrap()
    }
    fn indices(bytes: &[u8], kind: Kind) -> Vec<u32> {
        (0..word(bytes, 16))
            .filter(|index| field(bytes, *index, 0) == kind as u32)
            .collect()
    }
    fn validate(source: &str, bytes: &[u8]) {
        assert_eq!(&bytes[..8], MAGIC);
        assert_eq!(word(bytes, 8), VERSION);
        assert_eq!(word(bytes, 12) as usize, source.len());
        let count = word(bytes, 16);
        assert_eq!(
            bytes.len(),
            HEADER_BYTES + word(bytes, 28) as usize + count as usize * 4 + word(bytes, 20) as usize
        );
        let mut stack = Vec::new();
        for index in 0..count {
            while stack
                .last()
                .is_some_and(|parent| field(bytes, *parent, 2) == index)
            {
                stack.pop();
            }
            assert_eq!(field(bytes, index, 1), *stack.last().unwrap_or(&NO_PARENT));
            let end = field(bytes, index, 2);
            assert!(end > index && end <= count);
            if let Some(parent) = stack.last() {
                assert!(end <= field(bytes, *parent, 2));
            }
            let start = field(bytes, index, 3) as usize;
            let end_source = field(bytes, index, 4) as usize;
            assert!(start <= end_source && end_source <= source.len());
            assert!(source.is_char_boundary(start) && source.is_char_boundary(end_source));
            text(bytes, index);
            if field(bytes, index, 0) <= Kind::Image as u32 {
                stack.push(index);
            }
        }
    }

    #[test]
    fn global_tree_retains_nested_blocks_inline_semantics_and_reference_order() {
        let source = "> - [x] **中文** *斜体* ~~删除~~ `code` ![[alt][doc]](a.png) [正文][doc]\n>   - 子项\n\n3. 项目\n\n| 左 | 中 | 右 |\n|:--|:--:|--:|\n|甲|乙|丙|\n\n```cpp ignored\n<x>\n```\n\n---\n\n[doc]: other.md#title \"提示\"\n";
        let bytes = build(source).unwrap();
        validate(source, &bytes);
        for kind in [
            Kind::BlockQuote,
            Kind::BulletList,
            Kind::OrderedList,
            Kind::Item,
            Kind::TaskMarker,
            Kind::Strong,
            Kind::Emphasis,
            Kind::Strikethrough,
            Kind::InlineCode,
            Kind::Image,
            Kind::Link,
            Kind::Table,
            Kind::TableHead,
            Kind::TableRow,
            Kind::TableCell,
            Kind::CodeBlock,
            Kind::Rule,
        ] {
            assert!(!indices(&bytes, kind).is_empty(), "{kind:?}");
        }
        let links = indices(&bytes, Kind::Link);
        assert_eq!(links.len(), 2); // One in image alt, one visible: same global numbering.
        for (ordinal, index) in links.into_iter().enumerate() {
            assert_eq!(field(&bytes, index, 9), ordinal as u32);
            assert_eq!(text(&bytes, index), "other.md#title");
        }
        assert_eq!(text(&bytes, indices(&bytes, Kind::Table)[0]), "LCR");
        assert_eq!(text(&bytes, indices(&bytes, Kind::CodeBlock)[0]), "cpp");
        assert_eq!(field(&bytes, indices(&bytes, Kind::OrderedList)[0], 9), 3);
        assert_eq!(field(&bytes, indices(&bytes, Kind::TaskMarker)[0], 9), 1);
        assert_eq!(word(&bytes, 24), 1);
    }

    #[test]
    fn heading_ids_match_existing_map_with_duplicates_html_and_unicode() {
        let source = "# 中文 **加粗** `Code` ![图](x.png)\n# Foo\n# foo-1\n# FOO\n# foo\n#\n# !!!\nA <b>标签</b> &amp; [link](a.md)\n===\n";
        let bytes = build(source).unwrap();
        validate(source, &bytes);
        let expected = crate::headings::analyze(source).unwrap();
        let actual = indices(&bytes, Kind::Heading);
        assert_eq!(actual.len(), expected.len());
        for (index, heading) in actual.into_iter().zip(expected) {
            assert_eq!(text(&bytes, index), heading.id);
            assert_eq!(field(&bytes, index, 9), u32::from(heading.level));
            assert_eq!(field(&bytes, index, 3) as usize, heading.range.start);
            assert_eq!(field(&bytes, index, 4) as usize, heading.range.end);
        }
    }

    #[test]
    fn literal_content_breaks_and_derived_ranges_do_not_change_source() {
        let source = "中文😀e\u{301} &amp; \\*\r\n下一行  \n硬换行\u{2028}\u{2029} <b>x</b>\n\n<script>fetch('https://example.invalid')</script>\n\n` a  b `";
        let original = source.to_owned();
        let bytes = build(source).unwrap();
        validate(source, &bytes);
        assert_eq!(source, original);
        assert_eq!(indices(&bytes, Kind::SoftBreak).len(), 1);
        assert_eq!(indices(&bytes, Kind::HardBreak).len(), 1);
        assert_eq!(indices(&bytes, Kind::LiteralBlock).len(), 1);
        let literal: String = indices(&bytes, Kind::LiteralHtml)
            .into_iter()
            .map(|index| text(&bytes, index))
            .collect();
        assert!(literal.contains("<script>fetch("));
        assert!(!indices(&bytes, Kind::Text).is_empty());
        assert_eq!(text(&bytes, indices(&bytes, Kind::InlineCode)[0]), "a  b");
    }

    #[test]
    fn large_blocks_are_kept_whole_without_h1_partition_or_truncation() {
        for flavor in ["paragraph", "code", "table"] {
            let source = match flavor {
                "paragraph" => "x".repeat(MAX_SOURCE_BYTES),
                "code" => format!("```\n{}\n```", "x".repeat(MAX_SOURCE_BYTES - 8)),
                _ => {
                    let mut source = String::from("|a|b|\n|--|--|\n");
                    while source.len() + 6 <= MAX_SOURCE_BYTES {
                        source.push_str("|x|y|\n");
                    }
                    source
                }
            };
            let bytes = build(&source).unwrap();
            validate(&source, &bytes);
            assert!(indices(&bytes, Kind::Heading).is_empty());
            let root = if flavor == "paragraph" {
                Kind::Paragraph
            } else if flavor == "code" {
                Kind::CodeBlock
            } else {
                Kind::Table
            };
            assert_eq!(indices(&bytes, root).len(), 1);
            assert_eq!(field(&bytes, 0, 2), word(&bytes, 16));
            if flavor != "table" {
                let content_bytes: usize = indices(&bytes, Kind::Text)
                    .into_iter()
                    .map(|index| text(&bytes, index).len())
                    .sum();
                assert!(content_bytes >= MAX_SOURCE_BYTES - 8);
            }
        }
    }

    #[test]
    fn deep_nesting_is_an_iterative_tree_not_a_recursive_serializer() {
        let source = format!("{}text\n", "> ".repeat(4096));
        let bytes = build(&source).unwrap();
        validate(&source, &bytes);
        assert_eq!(indices(&bytes, Kind::BlockQuote).len(), 4096);
    }

    #[test]
    fn output_budget_fails_atomically_and_reference_guard_is_visible() {
        let source = "# 中文\n\n[x](target.md)\n";
        let full = build(source).unwrap();
        assert_eq!(build_with_limit(source, full.len()).unwrap(), full);
        assert_eq!(
            build_with_limit(source, full.len() - 1),
            Err(RenderError::OutputLimit)
        );
        assert_eq!(
            build_with_limit("", HEADER_BYTES - 1),
            Err(RenderError::OutputLimit)
        );
        assert_eq!(
            build(&"x".repeat(MAX_SOURCE_BYTES + 1)),
            Err(RenderError::SourceLimit)
        );
        let expansion = format!("{}\n\n[x]: {}\n", "[x] ".repeat(1000), "a".repeat(2000));
        assert_eq!(word(&build(&expansion).unwrap(), 24), 0);
    }

    #[test]
    fn ffi_validates_input_and_owns_exactly_one_releasable_buffer() {
        let invalid = [0xff];
        // SAFETY: all non-null inputs below are live immutable arrays.
        unsafe {
            let mut bad = crate::xdnote_markdown_native_model(invalid.as_ptr(), 1);
            assert_eq!(bad.status, 1);
            assert!(bad.data.is_null());
            crate::xdnote_markdown_free(&mut bad);
            assert_eq!(
                crate::xdnote_markdown_native_model(std::ptr::null(), 1).status,
                1
            );
            assert_eq!(
                crate::xdnote_markdown_native_model(std::ptr::null(), MAX_SOURCE_BYTES + 1).status,
                2
            );
            let mut empty = crate::xdnote_markdown_native_model(std::ptr::null(), 0);
            assert_eq!(empty.status, 0);
            assert_eq!(empty.length, HEADER_BYTES);
            crate::xdnote_markdown_free(&mut empty);
            crate::xdnote_markdown_free(&mut empty);
            assert!(empty.data.is_null());
            assert_eq!(empty.length, 0);
        }
    }
}
