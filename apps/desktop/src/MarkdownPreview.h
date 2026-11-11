#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QPointF>
#include <QStringList>
#include <QUuid>
#include <QWidget>
#include <QFileSystemWatcher>
#include <QTimer>
#include <memory>
#include "WorkspaceImagePreview.h"
#include "NativePreviewView.h"

namespace xdnote {
class DocumentSession;
class Appearance;
class WorkspaceResourceReader;
struct RenderResult {
    std::shared_ptr<const NativePreviewContent> content;
    QHash<int, QByteArray> images;
    QString error;
    quint64 generation = 0;
    quint64 revision = 0;
    QStringList linkTargets;
    QStringList imageWatchPaths;
    NativePreviewStamp stamp;
    bool localDocumentsAdmitted = false;
};

class MarkdownPreview final : public QWidget {
    Q_OBJECT
public:
    explicit MarkdownPreview(DocumentSession *session, QWidget *parent = nullptr, int padding = 24, Appearance *appearance = nullptr);
    ~MarkdownPreview() override;
    NativePreviewView *view() const { return m_view; }
    quint64 requestedGeneration() const { return m_generation; }
    quint64 displayedRevision() const { return m_displayedRevision; }
    void refresh();
    void setResourceContext(std::shared_ptr<WorkspaceResourceReader> root, const QString &documentPath, const QString &documentIdentity,
                            const AuthorizedPreviewImages &authorizedImages = {});
    QPointF readingPosition() const { return m_scroll; }
    void restoreReadingPosition(const QPointF &position);
    void navigateToHeading(const QString &fragment, quint64 revision);
signals:
    void previewReady(quint64 revision);
    void previewPrepared(quint64 revision);
    void previewError(const QString &message);
    void copyTextRequested(const QString &text);
    void fragmentNavigationMessage(const QString &message);
    void fragmentNavigated(const QString &fragment);
    void documentLinkRequested(QString target, quint64 revision);
private:
    void startPending();
    QPointer<DocumentSession> m_session;
    NativePreviewView *m_view = nullptr;
    QFutureWatcher<RenderResult> m_worker;
    QByteArray m_pending;
    QUuid m_pendingDocumentId;
    std::shared_ptr<WorkspaceResourceReader> m_resourceRoot;
    AuthorizedPreviewImages m_authorizedImages;
    QFileSystemWatcher m_imageWatch;
    QTimer m_imageDelay;
    QString m_resourceDocumentPath, m_resourceDocumentIdentity;
    QStringList m_presentingLinkTargets;
    quint64 m_pendingRevision = 0;
    quint64 m_generation = 0;
    quint64 m_contextGeneration = 0, m_resourceGeneration = 0;
    quint64 m_presentingGeneration = 0;
    quint64 m_displayedRevision = 0;
    bool m_hasPending = false;
    bool m_running = false;
    bool m_closed = false;
    QPointer<Appearance> m_appearance;
    QPointF m_scroll;
    bool m_restoring = false;
    QString m_pendingFragment;
    quint64 m_navigationRevision = 0;
    bool m_navigationPending = false;
    bool m_pageReady = false;
};
}
