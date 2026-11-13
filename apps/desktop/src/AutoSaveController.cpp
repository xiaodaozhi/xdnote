#include "AutoSaveController.h"
#include "Preferences.h"
#include "FileManager.h"
#include <QApplication>
#include <QTimer>

namespace xdnote {
AutoSaveController::AutoSaveController(FileManager *manager, Preferences *preferences, QObject *parent)
    : QObject(parent), m_preferences(preferences) {
    connect(manager, &FileManager::opened, this, [this](QUuid, FileDocument *document, bool) { watch(document); });
    for (auto *document : manager->documents()) watch(document);
    connect(preferences, &Preferences::changed, this, [this](const QString &section) {
        if (section == "editor") for (auto *document : m_timers.keys()) schedule(document);
    });
}
void AutoSaveController::watch(FileDocument *document) {
    if (!document || m_timers.contains(document)) return;
    auto *timer = new QTimer(this); timer->setSingleShot(true); m_timers.insert(document, timer);
    connect(document, &QObject::destroyed, this, [this, document, timer] { m_timers.remove(document); timer->stop(); timer->deleteLater(); });
    connect(document->session(), &DocumentSession::changed, this, [this, document] { schedule(document); });
    connect(document, &FileDocument::operationFinished, this, [this, document](bool success) { if (success) schedule(document); });
    connect(document->session(), &DocumentSession::stateChanged, this, [this, document] { schedule(document); });
    connect(timer, &QTimer::timeout, this, [this, document] {
        if (m_stopped || !m_preferences->value("editor", "autoSaveEnabled").toBool()) return;
        if (document->untitled() || !document->session() || document->closed() || !document->dirty() || document->conflict() || document->hasError() || document->readOnly()) return;
        if (document->saving() || document->loading() || document->maintainingResources() || document->session()->isBusy()
            || document->session()->isComposing() || QApplication::activeModalWidget()) { schedule(document); return; }
        document->saveAutomatically();
    });
}
void AutoSaveController::schedule(FileDocument *document) {
    auto *timer = m_timers.value(document); if (!timer) return;
    timer->stop();
    if (!m_stopped && m_preferences->value("editor", "autoSaveEnabled").toBool() && !document->closed()
        && document->dirty() && !document->conflict() && !document->hasError() && !document->readOnly())
        timer->start(m_preferences->value("editor", "autoSaveDelayMs").toInt());
}
void AutoSaveController::stop() { m_stopped = true; for (auto *timer : m_timers) timer->stop(); }
}
