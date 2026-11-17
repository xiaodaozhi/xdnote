#pragma once
#include <QString>
#include <QUrl>

namespace xdnote::UpdateNetwork {
inline bool validProxy(const QString &text) {
    if (text.isEmpty() || text.size() > 2048)
        return false;
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "socks5") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() &&
           !url.hasFragment() && url.port(-1) > 0 && url.port(-1) <= 65535 &&
           (url.path().isEmpty() || url.path() == "/");
}
inline bool macosAutomaticTransport(const QString &mode, const QString &address) {
    return mode == "system" || mode == "none" || (mode == "custom" && validProxy(address));
}
inline bool validMirror(const QString &text) {
    if (text.isEmpty())
        return true;
    if (text.size() > 2048)
        return false;
    const QUrl url(text, QUrl::StrictMode);
    const auto path = url.path(QUrl::FullyEncoded);
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
           (url.port(-1) == -1 || url.port(-1) == 443) && !path.contains('%') &&
           !path.split('/').contains(QStringLiteral("..")) && !path.contains('\\');
}
inline QUrl mapped(const QUrl &original, const QString &mirror) {
    if (mirror.isEmpty())
        return original;
    auto prefix = mirror;
    while (prefix.endsWith('/'))
        prefix.chop(1);
    return QUrl(prefix + '/' + original.toString(QUrl::FullyEncoded), QUrl::StrictMode);
}
inline bool isMirrorUrl(const QUrl &url, const QString &mirror) {
    if (mirror.isEmpty() || !validMirror(mirror) || !url.isValid() || url.scheme() != "https" ||
        !url.userInfo().isEmpty() || url.hasFragment())
        return false;
    const QUrl base(mirror, QUrl::StrictMode);
    auto prefix = base.path(QUrl::FullyEncoded);
    while (prefix.endsWith('/'))
        prefix.chop(1);
    const auto path = url.path(QUrl::FullyEncoded);
    for (const auto &segment : path.split('/')) {
        const auto decoded = QUrl::fromPercentEncoding(segment.toUtf8());
        if (decoded == "." || decoded == ".." || decoded.contains('/') || decoded.contains('\\'))
            return false;
    }
    return url.host().compare(base.host(), Qt::CaseInsensitive) == 0 &&
           url.port(443) == base.port(443) && path.startsWith(prefix + '/');
}
} // namespace UpdateNetwork
