//! Script-free, resource-free admission rendering. The original UTF-8 remains
//! owned by the desktop document session; this module only consumes snapshots.

use pulldown_cmark::{html, CodeBlockKind, CowStr, Event, Options, Parser, Tag, TagEnd};
use std::io::{self, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub const MAX_SOURCE_BYTES: usize = 2 * 1024 * 1024;
pub const MAX_HTML_BYTES: usize = 8 * 1024 * 1024;

#[derive(Debug, PartialEq, Eq)]
pub enum RenderError {
    SourceLimit,
    OutputLimit,
}

struct LimitedOutput(Vec<u8>);

impl Write for LimitedOutput {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > MAX_HTML_BYTES.saturating_sub(self.0.len()) {
            return Err(io::Error::other("preview output limit"));
        }
        self.0.extend_from_slice(bytes);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Produce a body fragment with no document-controlled HTML, URLs or resources.
/// Links and images are deliberately inert placeholders until resource and
/// navigation admission is implemented. This is not a general HTML sanitizer.
pub fn render(source: &str) -> Result<Vec<u8>, RenderError> {
    if source.len() > MAX_SOURCE_BYTES {
        return Err(RenderError::SourceLimit);
    }
    let options =
        Options::ENABLE_TABLES | Options::ENABLE_TASKLISTS | Options::ENABLE_STRIKETHROUGH;
    let mut parser = Parser::new_ext(source, options);
    let mut events = Vec::new();
    let mut code_index = 0;
    while let Some(event) = parser.next() {
        match event {
            Event::Start(Tag::CodeBlock(kind)) => {
                let language = match kind {
                    CodeBlockKind::Fenced(info) => {
                        info.split_whitespace().next().unwrap_or("").to_owned()
                    }
                    CodeBlockKind::Indented => String::new(),
                };
                let mut code = String::new();
                for part in parser.by_ref() {
                    match part {
                        Event::End(TagEnd::CodeBlock) => break,
                        Event::Text(text) => code.push_str(&text),
                        _ => {}
                    }
                }
                events.push(Event::Html(CowStr::from(code_block(
                    &language, &code, code_index,
                )?)));
                code_index += 1;
            }
            Event::Start(Tag::Table(columns)) => {
                events.push(Event::Html(CowStr::Borrowed("<div class=\"table-scroll\" tabindex=\"0\" role=\"region\" aria-label=\"表格，可横向滚动\">")));
                events.push(Event::Start(Tag::Table(columns)));
            }
            Event::End(TagEnd::Table) => {
                events.push(Event::End(TagEnd::Table));
                events.push(Event::Html(CowStr::Borrowed("</div>")));
            }
            event => events.push(match event {
                Event::Html(value) | Event::InlineHtml(value) => Event::Text(value),
                Event::Start(Tag::Link { .. }) => {
                    Event::Html(CowStr::Borrowed("<span class=\"link-placeholder\">"))
                }
                Event::End(TagEnd::Link) => Event::Html(CowStr::Borrowed("</span>")),
                Event::Start(Tag::Image { .. }) => {
                    Event::Html(CowStr::Borrowed("<span class=\"image-placeholder\">图片："))
                }
                Event::End(TagEnd::Image) => Event::Html(CowStr::Borrowed("</span>")),
                other => other,
            }),
        }
    }
    let mut output = LimitedOutput(Vec::new());
    html::write_html_io(&mut output, events.into_iter()).map_err(|_| RenderError::OutputLimit)?;
    Ok(output.0)
}

fn escaped(output: &mut LimitedOutput, value: &str) -> io::Result<()> {
    for part in value.split_inclusive(['&', '<', '>', '"', '\'']) {
        if let Some(last) = part.chars().last() {
            let replacement = match last {
                '&' => "&amp;",
                '<' => "&lt;",
                '>' => "&gt;",
                '"' => "&quot;",
                '\'' => "&#39;",
                _ => "",
            };
            if !replacement.is_empty() {
                output.write_all(&part.as_bytes()[..part.len() - last.len_utf8()])?;
                output.write_all(replacement.as_bytes())?;
                continue;
            }
        }
        output.write_all(part.as_bytes())?;
    }
    Ok(())
}

fn code_block(language: &str, code: &str, index: usize) -> Result<String, RenderError> {
    let mut output = LimitedOutput(Vec::new());
    let result = (|| -> io::Result<()> {
        output.write_all(
            b"<figure class=\"code-block\"><figcaption><span class=\"code-language\">",
        )?;
        escaped(
            &mut output,
            if language.is_empty() {
                "纯文本"
            } else {
                language
            },
        )?;
        write!(output, "</span><button type=\"button\" data-xdnote-code=\"{index}\" aria-label=\"复制代码\">复制</button></figcaption><pre tabindex=\"0\" aria-label=\"代码，可横向滚动\"><code>")?;
        // A small bounded lexer, not a second Markdown parser or executable
        // syntax engine. Unknown/large blocks stay literal and copy identically.
        let highlight = code.len() <= 128 * 1024
            && matches!(
                language.to_ascii_lowercase().as_str(),
                "rust"
                    | "rs"
                    | "cpp"
                    | "c"
                    | "c++"
                    | "javascript"
                    | "js"
                    | "typescript"
                    | "ts"
                    | "python"
                    | "py"
                    | "json"
                    | "css"
                    | "sh"
                    | "bash"
            );
        if highlight {
            highlight_code(&mut output, code)?;
        } else {
            escaped(&mut output, code)?;
        }
        output.write_all(b"</code></pre></figure>")
    })();
    result.map_err(|_| RenderError::OutputLimit)?;
    String::from_utf8(output.0).map_err(|_| RenderError::OutputLimit)
}

fn highlight_code(output: &mut LimitedOutput, code: &str) -> io::Result<()> {
    let mut position = 0;
    while position < code.len() {
        let rest = &code[position..];
        let first = rest.chars().next().unwrap();
        let (length, class) = if rest.starts_with("//") {
            (rest.find('\n').unwrap_or(rest.len()), "syntax-comment")
        } else if first == '"' || first == '\'' || first == '`' {
            let mut length = first.len_utf8();
            let mut escape = false;
            for ch in rest[first.len_utf8()..].chars() {
                length += ch.len_utf8();
                if !escape && ch == first {
                    break;
                }
                escape = !escape && ch == '\\';
            }
            (length, "syntax-string")
        } else if first.is_ascii_digit() {
            (
                rest.chars()
                    .take_while(|c| c.is_ascii_alphanumeric() || *c == '.' || *c == '_')
                    .count(),
                "syntax-number",
            )
        } else if first.is_ascii_alphabetic() || first == '_' {
            let length = rest
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
                .count();
            let word = &rest[..length];
            let keyword = matches!(
                word,
                "fn" | "let"
                    | "mut"
                    | "pub"
                    | "use"
                    | "struct"
                    | "impl"
                    | "return"
                    | "if"
                    | "else"
                    | "for"
                    | "while"
                    | "const"
                    | "class"
                    | "function"
                    | "import"
                    | "from"
                    | "def"
                    | "true"
                    | "false"
                    | "null"
                    | "None"
                    | "int"
                    | "void"
                    | "auto"
                    | "async"
                    | "await"
            ) || rest[length..].trim_start().starts_with(':');
            (length, if keyword { "syntax-keyword" } else { "" })
        } else {
            (first.len_utf8(), "")
        };
        if !class.is_empty() {
            write!(output, "<span class=\"{class}\">")?;
        }
        escaped(output, &rest[..length])?;
        if !class.is_empty() {
            output.write_all(b"</span>")?;
        }
        position += length;
    }
    Ok(())
}

/// C ABI ownership: successful data is allocated by Rust and must be released
/// exactly once with xdnote_markdown_free. The caller must not copy ownership.
#[repr(C)]
pub struct RenderBuffer {
    pub data: *mut u8,
    pub length: usize,
    pub status: u32,
}

impl RenderBuffer {
    fn error(status: u32) -> Self {
        Self {
            data: std::ptr::null_mut(),
            length: 0,
            status,
        }
    }
}

/// # Safety
/// A non-empty input points to `length` readable bytes for the duration of this
/// call. The desktop passes an owned immutable snapshot, not an editor pointer.
#[no_mangle]
pub unsafe extern "C" fn xdnote_markdown_render(data: *const u8, length: usize) -> RenderBuffer {
    if length > MAX_SOURCE_BYTES {
        return RenderBuffer::error(2);
    }
    if data.is_null() && length != 0 {
        return RenderBuffer::error(1);
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let bytes = if length == 0 {
            &[]
        } else {
            // SAFETY: the caller supplies one immutable, live snapshot.
            unsafe { std::slice::from_raw_parts(data, length) }
        };
        let source = std::str::from_utf8(bytes).map_err(|_| 1u32)?;
        let output = render(source).map_err(|_| 2u32)?;
        let length = output.len();
        let data = Box::into_raw(output.into_boxed_slice()) as *mut u8;
        Ok::<_, u32>(RenderBuffer {
            data,
            length,
            status: 0,
        })
    }));
    match result {
        Ok(Ok(buffer)) => buffer,
        Ok(Err(status)) => RenderBuffer::error(status),
        Err(_) => RenderBuffer::error(3),
    }
}

/// # Safety
/// `buffer` is null or points to the original unmodified return value of
/// xdnote_markdown_render. It must not have been freed through a copied struct.
#[no_mangle]
pub unsafe extern "C" fn xdnote_markdown_free(buffer: *mut RenderBuffer) {
    if buffer.is_null() {
        return;
    }
    // SAFETY: the caller owns this return struct exclusively.
    let buffer = unsafe { &mut *buffer };
    if !buffer.data.is_null() {
        let allocation = std::ptr::slice_from_raw_parts_mut(buffer.data, buffer.length);
        // SAFETY: this is the boxed slice returned by the render function.
        unsafe { drop(Box::from_raw(allocation)) };
    }
    *buffer = RenderBuffer::error(0);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn output(source: &str) -> String {
        String::from_utf8(render(source).unwrap()).unwrap()
    }

    #[test]
    fn core_markdown_and_extensions_are_rendered() {
        let text = "# 标题\n\n**粗体** ~~删除~~\n\n- [x] 完成\n\n|列|\n|-|\n|值|\n";
        let html = output(text);
        for fragment in [
            "<h1>标题</h1>",
            "<strong>粗体</strong>",
            "<del>删除</del>",
            "<table>",
        ] {
            assert!(html.contains(fragment), "missing {fragment}");
        }
        assert!(html.contains("type=\"checkbox\""));
        assert!(html.contains("disabled=\"\""));
    }

    #[test]
    fn document_html_is_text_and_urls_never_enter_markup() {
        let text = "<script>danger()</script>\n\n<img src=x onerror=alert(1)>\n\n[x](javascript:alert) ![说明](file:///private/image.png)";
        let html = output(text);
        assert!(!html.contains("<script>"));
        assert!(!html.contains("<img "));
        assert!(!html.contains("href="));
        assert!(!html.contains("javascript:"));
        assert!(!html.contains("file:///private"));
        assert!(html.contains("&lt;script&gt;"));
        assert!(html.contains("图片：说明"));
    }

    #[test]
    fn code_and_unicode_do_not_mutate_the_snapshot() {
        let text = "中文😀\u{a0}\u{2028}\u{2029}\r\n\n`<script>`\n";
        let before = text.as_bytes().to_vec();
        let html = output(text);
        assert_eq!(text.as_bytes(), before);
        assert!(html.contains("中文😀"));
        assert!(html.contains("<code>&lt;script&gt;</code>"));
    }

    #[test]
    fn code_chrome_escapes_language_and_preserves_literal_text() {
        let html = output("```js\nconst text = '<img onerror=x>';\n// 中文😀\n```\n\n```unknown\n<script>x</script>\n```\n");
        assert!(html.contains("data-xdnote-code=\"0\""));
        assert!(html.contains("data-xdnote-code=\"1\""));
        assert!(html.contains("class=\"syntax-keyword\">const</span>"));
        assert!(html.contains("&lt;img onerror=x&gt;"));
        assert!(!html.contains("<script>"));
        assert!(!html.contains("<img "));
        assert!(html.contains("// 中文😀"));
        let hostile = output("```\"onmouseover=x\n&<>\n```\n");
        assert!(hostile.contains("&quot;onmouseover=x"));
        assert!(!hostile.contains("<span class=\"code-language\">\""));
    }

    #[test]
    fn tables_have_scroll_region_without_losing_alignment() {
        let html = output("|左|右|\n|:--|--:|\n|一|二|\n");
        assert!(html.contains("class=\"table-scroll\""));
        assert!(html.contains("text-align: left"));
        assert!(html.contains("text-align: right"));
        assert!(html.contains("</table>\n</div>"));
    }

    #[test]
    fn limits_fail_without_partial_results() {
        assert_eq!(
            render(&"x".repeat(MAX_SOURCE_BYTES + 1)),
            Err(RenderError::SourceLimit)
        );
        assert_eq!(
            render(&"&".repeat(MAX_SOURCE_BYTES)),
            Err(RenderError::OutputLimit)
        );
    }

    #[test]
    fn ffi_validates_utf8_and_releases_its_own_allocation() {
        // SAFETY: these arrays and return structs remain alive during each call.
        unsafe {
            let bad = [0xff];
            assert_eq!(xdnote_markdown_render(bad.as_ptr(), bad.len()).status, 1);
            assert_eq!(xdnote_markdown_render(std::ptr::null(), 1).status, 1);
            let mut empty = xdnote_markdown_render(std::ptr::null(), 0);
            assert_eq!(empty.status, 0);
            xdnote_markdown_free(&mut empty);
            let mut result = xdnote_markdown_render(b"# hello".as_ptr(), 7);
            assert_eq!(result.status, 0);
            assert!(!result.data.is_null());
            xdnote_markdown_free(&mut result);
            assert!(result.data.is_null());
            assert_eq!(result.length, 0);
            xdnote_markdown_free(&mut result);
        }
    }
}
