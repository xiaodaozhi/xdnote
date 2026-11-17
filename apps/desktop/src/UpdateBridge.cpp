#include "UpdateBridge.h"
#include <QJsonDocument>
#include <cstddef>
#include <cstdint>
extern "C" {
struct XdnoteUpdateBuffer { unsigned char *data; std::size_t length; std::uint32_t status; };
XdnoteUpdateBuffer xdnote_update_request(const unsigned char *, std::size_t);
void xdnote_update_free(XdnoteUpdateBuffer *);
}
namespace xdnote {
QJsonObject updateRequest(const QJsonObject &request) {
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    auto buffer = xdnote_update_request(reinterpret_cast<const unsigned char *>(bytes.constData()), std::size_t(bytes.size()));
    QJsonObject result;
    if (buffer.status == 0 && buffer.data && buffer.length <= 64 * 1024)
        result = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(buffer.data), qsizetype(buffer.length))).object();
    xdnote_update_free(&buffer);
    if (result.isEmpty()) return {{"error", "update_bridge"}};
    return result;
}
}
