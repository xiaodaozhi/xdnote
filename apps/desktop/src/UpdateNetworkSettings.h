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
inline bool validConfiguration(const QString &mode, const QString &proxy, const QString &mirror) {
    return (mode == "system" || mode == "none" || mode == "custom") &&
           (proxy.isEmpty() || validProxy(proxy)) && (mode != "custom" || validProxy(proxy)) && validMirror(mirror);
}
} // namespace xdnote::UpdateNetwork
