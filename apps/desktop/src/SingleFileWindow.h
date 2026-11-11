#pragma once
#include "FileManager.h"
#include "WindowChrome.h"
#include "WorkspaceImagePreview.h"
#include <QImage>

class QLabel;
class QStackedWidget;
class QMenu;
class QAction;
class QPushButton;
class QCloseEvent;
class QDialog;
class QSplitter;
class QHBoxLayout;
namespace xdnote {
class SourceEditor;
class MarkdownPreview;
class ScrollSyncController;
class WindowTitleBar;
class Appearance;
class SharedButton;
class ElidedLabel;
class WindowGeometry;
class WindowGeometryStore;
class SharedSpinner;
class DocumentBreadcrumb;
class DocumentBody;
class WorkspaceResourceReader;
class SingleFileWindow final : public ApplicationWindow {
    Q_OBJECT
public:
    explicit SingleFileWindow(FileManager *manager, Appearance *appearance = nullptr, WindowGeometryStore *geometryStore = nullptr);
    ~SingleFileWindow() override;
    enum class ViewMode { Reading, Source, Split };
    QUuid paneId() const { return m_paneId; }
    QString displayPath() const;
    bool embedded() const { return m_embedded; }
    bool canTransfer() const;
    bool embedIn(QWidget *parent);
    void setWorkspaceRoot(const QString &root);
    QWidget *footer() const { return m_footer; }
    void activateDocument();
    void reportHostMessage(const QString &message);
    ViewMode viewMode() const { return m_mode; }
    void setViewMode(ViewMode mode);
    WindowGeometry *windowGeometry() const { return m_geometry; }
    FileDocument *document() const { return m_document; }
    SourceEditor *editor() const { return m_editor; }
    MarkdownPreview *preview() const { return m_preview; }
    void setLoading(const QString &path, const QUuid &intent);
    void showOpenError(const QString &path, const QString &message);
    void setDocument(FileDocument *document);
    void setWorkspaceResourceRoot(std::shared_ptr<WorkspaceResourceReader> root, const QString &path);
    bool editing() const { return m_editing; }
    void finishOpen() { m_intent = {}; }
    void setEditing(bool editing);
    void resolveConflict();
    void chooseSaveAs();
    Appearance *appearance() const { return m_appearance; }
    void setContentZoom(int percent);
    int contentZoom() const { return m_zoom; }
    void importImageFile(const QString &path, const QString &destinationDirectory = {});
    void importClipboardImage(const QImage &image, const QString &destinationDirectory = {});
    void insertWorkspaceDocumentLink(const QString &targetPath, const QString &heading = {}, bool chooseHeading = false);
    void activateDocumentLink(const QString &target, quint64 revision);
    void navigateToHeading(const QString &fragment);
    void navigateToReference(const QString &identity, const QByteArray &sha256, qsizetype start, qsizetype end);
    std::shared_ptr<WorkspaceResourceReader> resourceRoot() const { return m_embedded ? m_workspaceResourceRoot : m_localResourceRoot; }
    QString resourceDocumentPath() const;
signals:
    void openRequested(QString path);
    void cancelOpenRequested(QUuid intent);
    void quitRequested();
    void closeCancelled();
    void copyPathRequested(QString path);
    void copyTextRequested(QString text);
    void revealRequested(QString directory);
    void workspaceRequested();
    void openDirectoryRequested();
    void sidebarRequested();
    void viewStateChanged();
    void imageImportFinished(bool inserted, QString message);
    void workspaceLinkFinished(bool inserted, QString message);
    void referencesRequested(QMenu *menu);
    void documentLinkRequested(QString target, quint64 revision);
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    void updateState();
    void chooseOpen();
    void reloadDisk();
    void wrapSelection(const QString &before, const QString &after);
    void applyAppearance();
    void updateCaret();
    void showSaveError();
    void syncPreviewResources();
    ScrollSyncController *m_scrollSync = nullptr;
    void beginImageImport(const QString &path, const QImage &image, const QString &destinationDirectory = {});
    void chooseImage();
    void chooseWorkspaceDocumentLink();
    FileManager *m_manager;
    const QUuid m_paneId = QUuid::createUuid();
    Appearance *m_appearance;
    WindowGeometry *m_geometry = nullptr;
    QPointer<FileDocument> m_document;
    std::shared_ptr<WorkspaceResourceReader> m_workspaceResourceRoot, m_localResourceRoot;
    QString m_workspaceResourcePath, m_localResourcePath;
    AuthorizedPreviewImages m_importedImages;
    QString m_importedImageDocument;
    QUuid m_intent;
    WindowTitleBar *m_header;
    ElidedLabel *m_path;
    DocumentBreadcrumb *m_breadcrumb;
    QString m_workspaceRoot;
    QLabel *m_notice;
    ElidedLabel *m_status;
    QLabel *m_statusIcon;
    QLabel *m_cursor;
    QLabel *m_encoding;
    QLabel *m_empty;
    QLabel *m_emptyTitle, *m_emptySymbol, *m_noticeSymbol;
    SharedSpinner *m_openSpinner, *m_saveSpinner;
    SharedButton *m_cancelOpen, *m_welcomeOpen, *m_noticeClose;
    QWidget *m_previewFailure = nullptr;
    QLabel *m_previewFailureText = nullptr;
    QPointer<QDialog> m_saveErrorDialog;
    QWidget *m_banner;
    QWidget *m_editorPage = nullptr;
    DocumentBody *m_body;
    QStackedWidget *m_previewStack = nullptr;
    QSplitter *m_splitter = nullptr;
    QWidget *m_previewPane = nullptr, *m_previewHeading = nullptr, *m_footer = nullptr;
    QHBoxLayout *m_modeLayout;
    QPushButton *m_read;
    QPushButton *m_edit;
    QPushButton *m_split;
    QPushButton *m_save;
    QPushButton *m_resolve;
    QPushButton *m_retry;
    QPushButton *m_later;
    SharedButton *m_zoomButton;
    SharedButton *m_pathButton;
    QList<SharedButton *> m_formats;
    QMenu *m_menu;
    QAction *m_saveAction;
    QAction *m_saveAsAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    QAction *m_reloadAction;
    QAction *m_imageImportAction;
    QAction *m_linkInsertAction;
    QMenu *m_referencesMenu;
    QAction *m_openLinkAction;
    SourceEditor *m_editor = nullptr;
    MarkdownPreview *m_preview = nullptr;
    bool m_editing = false, m_prompting = false, m_closeAfterOperation = false;
    QString m_previewError;
    QString m_navigationMessage;
    QString m_hostMessage;
    QString m_appearanceError, m_geometryError, m_dismissedNotice, m_openPath, m_openError;
    int m_zoom = 100;
    bool m_observedSave = false;
    bool m_documentUiReady = false;
    bool m_imageImportPending = false;
    bool m_linkInsertPending = false;
    bool m_embedded = false;
    ViewMode m_mode = ViewMode::Reading;
};
}
