#pragma once

#include <QWidget>
#include <QPointer>

class QQuickWidget;

namespace xdnote {
class DocumentSession;
class SourceEditor;
class MarkdownPreview;

class AdmissionWindow final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY sessionStateChanged)
    Q_PROPERTY(QString feedback READ feedback NOTIFY sessionStateChanged)
public:
    explicit AdmissionWindow(DocumentSession *session, QWidget *parent = nullptr);
    ~AdmissionWindow() override;
    bool readOnly() const;
    QString feedback() const;
    SourceEditor *editor() const { return m_editor; }
    MarkdownPreview *preview() const { return m_preview; }
    QQuickWidget *chrome() const { return m_chrome; }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void toggleReadOnly();
signals:
    void sessionStateChanged();
private:
    QPointer<DocumentSession> m_session;
    SourceEditor *m_editor;
    MarkdownPreview *m_preview;
    QQuickWidget *m_chrome;
    QString m_previewError;
};
}
