#include "VisualTokens.h"
#include <QFile>
#include <QJsonDocument>
#include <QResource>
#include <stdexcept>

static void initializeXdnoteResources() { Q_INIT_RESOURCE(xdnote_assets); }

namespace xdnote {
const QJsonObject &visualTokens() {
    static const QJsonObject tokens = [] {
        initializeXdnoteResources();
        QFile file(QStringLiteral(":/xdnote/docs/visual/tokens.json"));
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("shared visual tokens missing");
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject() || document.object().value(QStringLiteral("unit")).toString() != QStringLiteral("logical-pixel"))
            throw std::runtime_error("invalid shared visual tokens");
        return document.object();
    }();
    return tokens;
}
}
