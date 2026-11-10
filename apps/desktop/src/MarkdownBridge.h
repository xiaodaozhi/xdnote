#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
struct XdnoteRenderBuffer { unsigned char *data; std::size_t length; std::uint32_t status; };
XdnoteRenderBuffer xdnote_markdown_render(const unsigned char *data, std::size_t length);
XdnoteRenderBuffer xdnote_markdown_render_images(const unsigned char *data, std::size_t length,
    const unsigned char *prefix, std::size_t prefixLength, const unsigned char *states, std::size_t statesLength);
XdnoteRenderBuffer xdnote_markdown_render_images_and_fragments(const unsigned char *data, std::size_t length,
    const unsigned char *prefix, std::size_t prefixLength, const unsigned char *states, std::size_t statesLength);
XdnoteRenderBuffer xdnote_markdown_render_images_and_links(const unsigned char *data, std::size_t length,
    const unsigned char *prefix, std::size_t prefixLength, const unsigned char *states, std::size_t statesLength);
XdnoteRenderBuffer xdnote_markdown_references(const unsigned char *data, std::size_t length);
XdnoteRenderBuffer xdnote_markdown_headings(const unsigned char *data, std::size_t length);
XdnoteRenderBuffer xdnote_workspace_resolve(const unsigned char *document, std::size_t documentLength, const unsigned char *target, std::size_t targetLength);
XdnoteRenderBuffer xdnote_workspace_relative_url(const unsigned char *document, std::size_t documentLength, const unsigned char *target, std::size_t targetLength);
void xdnote_markdown_free(XdnoteRenderBuffer *buffer);
}
