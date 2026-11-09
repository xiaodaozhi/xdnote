#include "WorkspacePath.h"
#include "MarkdownBridge.h"
#include "Utf8Text.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QScopeGuard>

namespace xdnote {
namespace {
using Operation = XdnoteRenderBuffer (*)(const unsigned char *, std::size_t, const unsigned char *, std::size_t);

WorkspacePathResult invoke(const QString &document, const QString &target, Operation operation) {
    WorkspacePathResult result;
    const auto first = document.toUtf8(), second = target.toUtf8();
    if (decodeUtf8Literal(first) != document || decodeUtf8Literal(second) != target) { result.error = "invalid-utf8"; return result; }
    auto buffer = operation(reinterpret_cast<const unsigned char *>(first.constData()), std::size_t(first.size()),
                            reinterpret_cast<const unsigned char *>(second.constData()), std::size_t(second.size()));
    const auto release = qScopeGuard([&] { xdnote_markdown_free(&buffer); });
    if (buffer.status != 0 || !buffer.data || buffer.length > 128 * 1024) { result.error = "analysis-failed"; return result; }
    QJsonParseError error{};
    const auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length)), &error);
    const auto object = json.object();
    if (error.error != QJsonParseError::NoError || !json.isObject() || object.value("version").toInt() != 1 || !object.value("ok").isBool()) {
        result.error = "invalid-result"; return result;
    }
    if (!object.value("ok").toBool()) {
        if (!object.value("error").isString()) { result.error = "invalid-result"; return result; }
        result.error = object.value("error").toString(); return result;
    }
    const auto path = object.value("path"), fragment = object.value("fragment");
    if (!path.isString() || !(fragment.isNull() || fragment.isString())) { result.error = "invalid-result"; return result; }
    result.success = true; result.path = path.toString(); result.hasFragment = fragment.isString(); result.fragment = fragment.toString();
    return result;
}
}
WorkspacePathResult resolveWorkspaceTarget(const QString &document, const QString &target) { return invoke(document, target, xdnote_workspace_resolve); }
WorkspacePathResult relativeWorkspaceUrl(const QString &document, const QString &destination) { return invoke(document, destination, xdnote_workspace_relative_url); }
}
