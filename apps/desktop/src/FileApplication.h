#pragma once
#include "FileManager.h"
#include <QSet>
namespace xdnote {
class SingleFileWindow;
class FileApplication final : public QObject {
    Q_OBJECT
public:
    explicit FileApplication(QObject *parent = nullptr);
    ~FileApplication() override;
    void open(const QString &path, SingleFileWindow *empty = nullptr);
    SingleFileWindow *emptyWindow();
    void activate();
    void requestQuit();
protected:
    bool eventFilter(QObject *target, QEvent *event) override;
private:
    SingleFileWindow *createWindow();
    void advanceQuit();
    FileManager m_manager;
    QSet<SingleFileWindow *> m_windows;
    QHash<QUuid, QPointer<SingleFileWindow>> m_requests;
    QHash<QUuid, QPointer<SingleFileWindow>> m_documents;
    bool m_quitting = false;
};
}
