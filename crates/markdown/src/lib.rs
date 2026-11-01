//! Script-free, resource-free admission rendering. The original UTF-8 remains
//! owned by the desktop document session; this module only consumes snapshots.

use pulldown_cmark::{html, CowStr, Event, Options, Parser, Tag, TagEnd};
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
    let events = Parser::new_ext(source, options).map(|event| match event {
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
    });
    let mut output = LimitedOutput(Vec::new());
    html::write_html_io(&mut output, events).map_err(|_| RenderError::OutputLimit)?;
    Ok(output.0)
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
