#include "FileApplication.h"
#include "SingleFileWindow.h"
#include <QApplication>
#include <QFileOpenEvent>
#include <QClipboard>
#include <QDesktopServices>

namespace xdnote {
FileApplication::FileApplication(QObject *parent, QString settingsPath) : QObject(parent), m_appearance(this, std::move(settingsPath)), m_manager(this) {
    qApp->installEventFilter(this);
    connect(&m_manager, &FileManager::opened, this, [this](QUuid intent, FileDocument *document, bool reused) {
        auto placeholder = m_requests.take(intent);
        if (!placeholder) { if (!reused) m_manager.closeDocument(document); return; }
        const auto id = document->session()->documentId();
        auto window = m_documents.value(id);
        if (window) { placeholder->finishOpen(); if (window != placeholder) placeholder->close(); if (window->isMinimized()) window->showNormal(); window->raise(); window->activateWindow(); return; }
        placeholder->setDocument(document); m_documents.insert(id, placeholder);
        connect(placeholder, &QObject::destroyed, this, [this, id] { m_documents.remove(id); });
        placeholder->show(); placeholder->raise(); placeholder->activateWindow();
    });
    connect(&m_manager, &FileManager::openFailed, this, [this](QUuid intent, const QString &path, const QString &message) {
        auto window = m_requests.take(intent); if (window) window->showOpenError(path, message);
    });
}
FileApplication::~FileApplication() {
    const auto windows = m_windows;
    for (auto *window : windows) delete window;
}
SingleFileWindow *FileApplication::createWindow() {
    auto *window = new SingleFileWindow(&m_manager, &m_appearance); m_windows.insert(window);
    connect(window, &SingleFileWindow::copyPathRequested, this, [](const QString &path) { QGuiApplication::clipboard()->setText(path); });
    connect(window, &SingleFileWindow::copyTextRequested, this, [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
    connect(window, &SingleFileWindow::revealRequested, this, [](const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    connect(window, &SingleFileWindow::openRequested, this, [this, window](const QString &path) { open(path, window->document() ? nullptr : window); });
    connect(window, &SingleFileWindow::cancelOpenRequested, this, [this](QUuid intent) { m_requests.remove(intent); m_manager.cancelOpen(intent); });
    connect(window, &SingleFileWindow::quitRequested, this, &FileApplication::requestQuit);
    connect(window, &SingleFileWindow::closeCancelled, this, [this] { m_quitting = false; });
    connect(window, &QObject::destroyed, this, [this, window] {
        m_windows.remove(window);
        if (m_windows.isEmpty()) QCoreApplication::quit();
        else if (m_quitting) QTimer::singleShot(0, this, &FileApplication::advanceQuit);
    });
    return window;
}
SingleFileWindow *FileApplication::emptyWindow() { auto *window = createWindow(); window->show(); return window; }
void FileApplication::open(const QString &path, SingleFileWindow *empty) {
    m_quitting = false;
    auto *window = empty && !empty->document() ? empty : createWindow();
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it.value() == window) { m_manager.cancelOpen(it.key()); it = m_requests.erase(it); } else ++it;
    }
    const auto intent = QUuid::createUuid(); m_requests.insert(intent, window); window->setLoading(path, intent); window->show(); m_manager.open(path, intent);
}
void FileApplication::activate() {
    if (m_windows.isEmpty()) { emptyWindow(); return; }
    auto *window = *m_windows.begin(); if (window->isMinimized()) window->showNormal(); window->raise(); window->activateWindow();
}
void FileApplication::requestQuit() { if (!m_quitting) { m_quitting = true; advanceQuit(); } }
void FileApplication::advanceQuit() {
    if (!m_quitting) return;
    for (auto *window : m_windows) if (window->isVisible()) { window->close(); return; }
    QCoreApplication::quit();
}
bool FileApplication::eventFilter(QObject *target, QEvent *event) {
    if (target == qApp && event->type() == QEvent::FileOpen) {
        const auto *file = static_cast<QFileOpenEvent *>(event);
        if (!file->file().isEmpty()) open(file->file()); else if (file->url().isLocalFile()) open(file->url().toLocalFile());
        return true;
    }
    if (target == qApp && event->type() == QEvent::Quit) {
        for (auto *window : m_windows) if (window->isVisible()) { requestQuit(); return true; }
    }
    return QObject::eventFilter(target, event);
}
}
