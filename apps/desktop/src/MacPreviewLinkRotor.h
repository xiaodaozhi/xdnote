#pragma once

#include <QAccessible>

namespace xdnote {
class NativePreviewView;

inline constexpr char MacPreviewAccessibleId[] = "xdnote.markdown.reader";
inline constexpr char MacPreviewLinkIdPrefix[] = "xdnote.markdown.link.";
inline QString macPreviewLinkId(QAccessible::Id id)
{
    return QString::fromLatin1(MacPreviewLinkIdPrefix) + QString::number(id);
}
void setMacPreviewLinkRotor(NativePreviewView *preview, bool visible);
void notifyMacPreviewNavigation(NativePreviewView *preview);
}
