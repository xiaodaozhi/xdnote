#include "VisualTokens.h"
#include <QFile>
#include <QJsonDocument>
#include <QResource>
#include <stdexcept>

#ifndef XDNOTE_RESOURCE_NAME
#define XDNOTE_RESOURCE_NAME xdnote_assets
#endif
#define XDNOTE_INITIALIZE_RESOURCE(name) Q_INIT_RESOURCE(name)
static void initializeXdnoteResources() { XDNOTE_INITIALIZE_RESOURCE(XDNOTE_RESOURCE_NAME); }

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
