#include "NavigationStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace xdnote {
namespace {
constexpr int recentLimit = 50, favoriteLimit = 200, byteLimit = 262144;
QString normalized(const QString &path) {
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) return {};
    const QFileInfo info(path); const auto suffix = info.suffix().toLower();
    if (suffix != "md" && suffix != "markdown") return {};
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
QString key(const QString &path) {
    auto value = path.isEmpty() ? QString() : QDir::cleanPath(path);
#ifdef Q_OS_WIN
    value = value.toCaseFolded();
#endif
    return value;
}
}
NavigationStore::NavigationStore(QObject *parent, QString path) : QObject(parent), m_path(std::move(path)) {
    if (m_path.isEmpty() || !QFileInfo::exists(m_path)) return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > byteLimit) { m_valid = false; m_error = QStringLiteral("无法读取导航记录，原文件已保留"); return; }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || document.object().value("schemaVersion").toInt() != 1) { m_valid = false; m_error = QStringLiteral("导航记录格式无效或版本不支持，原文件已保留"); return; }
    m_settings = document.object();
    for (const auto &value : m_settings.value("recent").toArray()) {
        const auto entry = value.toObject(); const auto path = normalized(entry.value("path").toString()); const auto opened = QDateTime::fromString(entry.value("opened").toString(), Qt::ISODateWithMs);
        if (path.isEmpty() || !opened.isValid() || std::any_of(m_recent.cbegin(), m_recent.cend(), [&path](const auto &item) { return key(item.path) == key(path); })) continue;
        m_recent.append({path, opened}); if (m_recent.size() == recentLimit) break;
    }
    for (const auto &value : m_settings.value("favorites").toArray()) {
        const auto path = normalized(value.toString()); if (path.isEmpty() || favorite(path)) continue;
        m_favorites.append(path); if (m_favorites.size() == favoriteLimit) break;
    }
}
bool NavigationStore::favorite(const QString &path) const { const auto identity = key(path); return !identity.isEmpty() && std::any_of(m_favorites.cbegin(), m_favorites.cend(), [&identity](const auto &item) { return key(item) == identity; }); }
bool NavigationStore::fail(const QString &message) { m_error = message; emit storageFailed(message); return false; }
bool NavigationStore::recordOpen(const QString &value) {
    const auto path = normalized(value); if (path.isEmpty()) return false;
    auto recent = m_recent; recent.erase(std::remove_if(recent.begin(), recent.end(), [&path](const auto &item) { return key(item.path) == key(path); }), recent.end());
    recent.prepend({path, QDateTime::currentDateTimeUtc()}); while (recent.size() > recentLimit) recent.removeLast(); return commit(recent, m_favorites);
}
bool NavigationStore::setFavorite(const QString &value, bool selected) {
    const auto path = normalized(value); if (path.isEmpty()) return fail(QStringLiteral("仅支持收藏 Markdown 文件"));
    if (favorite(path) == selected) return true;
    auto favorites = m_favorites;
    if (selected) { if (favorites.size() >= favoriteLimit) return fail(QStringLiteral("收藏已达 200 项，请先移除不再需要的收藏")); favorites.append(path); }
    else favorites.erase(std::remove_if(favorites.begin(), favorites.end(), [&path](const auto &item) { return key(item) == key(path); }), favorites.end());
    return commit(m_recent, favorites);
}
bool NavigationStore::commit(const QList<NavigationEntry> &recent, const QStringList &favorites) {
    if (!m_valid) return fail(m_error);
    auto settings = m_settings; QJsonArray entries, stars;
    for (const auto &entry : recent) entries.append(QJsonObject{{"path", entry.path}, {"opened", entry.opened.toUTC().toString(Qt::ISODateWithMs)}});
    for (const auto &path : favorites) stars.append(path);
    settings.insert("schemaVersion", 1); settings.insert("recent", entries); settings.insert("favorites", stars);
    if (!m_path.isEmpty()) {
        if (!QDir().mkpath(QFileInfo(m_path).path())) return fail(QStringLiteral("无法创建导航记录目录，更改未保存"));
        QSaveFile file(m_path); const auto bytes = QJsonDocument(settings).toJson();
        if (bytes.size() > byteLimit || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return fail(QStringLiteral("无法保存导航记录，更改未保存：") + file.errorString());
    }
    m_settings = settings; m_recent = recent; m_favorites = favorites; m_error.clear(); emit changed(); return true;
}
}
