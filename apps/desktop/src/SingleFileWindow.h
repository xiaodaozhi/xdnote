#pragma once
#include "FileManager.h"
#include <QWidget>

class QLabel;
class QStackedWidget;
class QMenu;
class QAction;
class QPushButton;
class QCloseEvent;
class QDialog;
namespace xdnote {
class SourceEditor;
class MarkdownPreview;
class DocumentHeader;
class Appearance;
class SharedButton;
class ElidedLabel;
class SharedSpinner;
class SingleFileWindow final : public QWidget {
    Q_OBJECT
public:
    explicit SingleFileWindow(FileManager *manager, Appearance *appearance = nullptr);
    FileDocument *document() const { return m_document; }
    SourceEditor *editor() const { return m_editor; }
    MarkdownPreview *preview() const { return m_preview; }
    void setLoading(const QString &path, const QUuid &intent);
    void showOpenError(const QString &path, const QString &message);
    void setDocument(FileDocument *document);
    bool editing() const { return m_editing; }
    void finishOpen() { m_intent = {}; }
    void setEditing(bool editing);
    void resolveConflict();
    void chooseSaveAs();
    Appearance *appearance() const { return m_appearance; }
    void setContentZoom(int percent);
    int contentZoom() const { return m_zoom; }
signals:
    void openRequested(QString path);
    void cancelOpenRequested(QUuid intent);
    void quitRequested();
    void closeCancelled();
    void copyPathRequested(QString path);
    void copyTextRequested(QString text);
    void revealRequested(QString directory);
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    void updateState();
    void chooseOpen();
    void reloadDisk();
    void wrapSelection(const QString &before, const QString &after);
    void applyAppearance();
    void updateCaret();
    void showSaveError();
    void applyNativeAppearance();
    FileManager *m_manager;
    Appearance *m_appearance;
    QPointer<FileDocument> m_document;
    QUuid m_intent;
    DocumentHeader *m_header;
    ElidedLabel *m_path;
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
    QStackedWidget *m_body;
    QPushButton *m_read;
    QPushButton *m_edit;
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
    QList<QAction *> m_themeActions, m_accentActions;
    SourceEditor *m_editor = nullptr;
    MarkdownPreview *m_preview = nullptr;
    bool m_editing = false, m_prompting = false, m_closeAfterOperation = false;
    QString m_previewError;
    QString m_appearanceError, m_dismissedNotice, m_openPath, m_openError;
    int m_zoom = 100;
    bool m_frameInitialized = false;
    bool m_nativeMaxPressed = false;
    bool m_observedSave = false;
};
}
