#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <memory>

class ScintillaEditBase;

namespace xdnote {
class SourceEditor;

enum class EditResult { Applied, Unchanged, ReadOnly, Composing, Busy, Closing, InvalidText, InvalidRange, WrongThread, StaleRevision };
QString resultDescription(EditResult result);

struct DocumentSnapshot {
    EditResult result = EditResult::Busy;
    QByteArray utf8;
    quint64 revision = 0;
};

// The hidden public Scintilla control owns the sole document independently of
// visible windows. Views attach to that document through Scintilla's reference
// counted public API. No QTextDocument or second writable text model is used.
class DocumentSession final : public QObject {
    Q_OBJECT
public:
    static constexpr qsizetype AdmissionSourceLimit = 2 * 1024 * 1024;
    explicit DocumentSession(const QByteArray &utf8 = {}, QObject *parent = nullptr);
    ~DocumentSession() override;
    DocumentSnapshot snapshot();
    EditResult replace(int startUtf16, int endUtf16, const QString &text, quint64 expectedRevision);
    EditResult undo();
    EditResult redo();
    EditResult setReadOnly(bool readOnly);
    EditResult requestClose();
    bool isReadOnly() const;
    bool isClosing() const { return m_closing; }
    bool isBusy() const;
    bool isComposing() const;
    quint64 revision();
    QString rejection() const { return m_rejection; }
    void reportRejection(EditResult result);
    static bool validText(const QString &text);
    static bool utf16Boundary(const QString &text, int offset);

signals:
    void changed(quint64 revision);
    void stateChanged();
    void commandRejected(xdnote::EditResult result);
    void closed();

private:
    friend class SourceEditor;
    void attach(SourceEditor *view);
    void inputFinished();
    void scheduleFlush();
    void flush();
    EditResult writable() const;
    QByteArray bytes() const;
    EditResult complete(EditResult result);
    void finishClose();
    std::unique_ptr<ScintillaEditBase> m_owner;
    QVector<QPointer<SourceEditor>> m_views;
    quint64 m_revision = 0;
    bool m_pending = false;
    bool m_flushQueued = false;
    bool m_notifying = false;
    bool m_reporting = false;
    bool m_closing = false;
    int m_mutations = 0;
    QString m_rejection;
};
}
Q_DECLARE_METATYPE(xdnote::EditResult)
