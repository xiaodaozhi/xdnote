#pragma once
#include "FileManager.h"
#include <QWidget>

class QLabel;
class QStackedWidget;
class QMenu;
class QAction;
class QPushButton;
class QCloseEvent;
namespace xdnote {
class SourceEditor;
class MarkdownPreview;
class DocumentHeader;
class SingleFileWindow final : public QWidget {
    Q_OBJECT
public:
    explicit SingleFileWindow(FileManager *manager);
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
signals:
    void openRequested(QString path);
    void cancelOpenRequested(QUuid intent);
    void quitRequested();
    void closeCancelled();
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    void updateState();
    void chooseOpen();
    void reloadDisk();
    void wrapSelection(const QString &before, const QString &after);
    FileManager *m_manager;
    QPointer<FileDocument> m_document;
    QUuid m_intent;
    DocumentHeader *m_header;
    QLabel *m_path;
    QLabel *m_notice;
    QLabel *m_status;
    QLabel *m_encoding;
    QLabel *m_empty;
    QWidget *m_banner;
    QWidget *m_editorPage = nullptr;
    QStackedWidget *m_body;
    QPushButton *m_read;
    QPushButton *m_edit;
    QPushButton *m_save;
    QPushButton *m_resolve;
    QMenu *m_menu;
    QAction *m_saveAction;
    QAction *m_saveAsAction;
    QAction *m_undoAction;
    QAction *m_redoAction;
    SourceEditor *m_editor = nullptr;
    MarkdownPreview *m_preview = nullptr;
    bool m_editing = false, m_prompting = false, m_closeAfterOperation = false;
    QString m_previewError;
};
}
