#pragma once
#include "WorkspaceImagePreview.h"
#include <QJsonObject>
namespace xdnote {
struct DocumentExportData {
    QByteArray html;
    QString error;
    QUuid document;
    quint64 revision=0;
};
DocumentExportData prepareDocumentExport(const DocumentSnapshot &snapshot, const QString &title,
    const std::shared_ptr<WorkspaceResourceReader> &root, const QString &path, const QString &identity,
    const AuthorizedPreviewImages &authorized, const QJsonObject &reader, const PreviewCancellation &cancel={});
// Atomic, explicit destination only. The source path/identity is always protected.
bool writeDocumentExport(const DocumentExportData &data, const QString &destination, bool pdf,
    const QString &sourcePath, const QString &sourceIdentity, QString *error);
}
