#pragma once
#include <QObject>
#include <QHash>
class QTimer;
namespace xdnote {
class FileManager;
class FileDocument;
class Preferences;
class AutoSaveController final : public QObject {
    Q_OBJECT
public:
    AutoSaveController(FileManager *manager, Preferences *preferences, QObject *parent);
    void stop();
private:
    void watch(FileDocument *document);
    void schedule(FileDocument *document);
    QHash<FileDocument *, QTimer *> m_timers;
    Preferences *m_preferences;
    bool m_stopped = false;
};
}
