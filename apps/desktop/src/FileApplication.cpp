#include "FileApplication.h"
#include "RecoveryStore.h"
#include "SharedWidgets.h"
#include <QVBoxLayout>
#include <QJsonArray>
#include <QScrollBar>
#include "Utf8Text.h"
#include "SingleFileWindow.h"
#include "WorkspaceWindow.h"
#include "WorkspaceTree.h"
#include "SourceEditor.h"
#include "MarkdownPreview.h"
#include "InputModality.h"
#include "WorkspaceResourceReader.h"
#include "WorkspacePath.h"
#include "WorkspaceReferenceController.h"
#include "SettingsWindow.h"
#include "AutoSaveController.h"
#include <QApplication>
#include <QFileOpenEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QKeyEvent>

namespace xdnote {
namespace {
QString rootKey(const QString &path) { const QFileInfo info(path); auto key = info.canonicalFilePath(); if (key.isEmpty()) key = info.absoluteFilePath();
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return QDir::cleanPath(key);
}
}
FileApplication::FileApplication(QObject *parent, QString settingsPath, Presentation presentation)
    : QObject(parent), m_presentation(presentation), m_appearance(this, settingsPath), m_geometryStore(this, settingsPath.isEmpty() ? QString() : settingsPath + ".windows"), m_navigation(this, settingsPath.isEmpty() ? QString() : settingsPath + ".navigation"), m_manager(this) {
    qApp->installEventFilter(this);
    m_recovery=new RecoveryStore(settingsPath.isEmpty()?QString():settingsPath+".recovery",this);
    const auto stored=m_recovery->state();m_lastSession=stored["documents"].toArray();
    for(const auto &value:m_lastSession){const auto item=value.toObject();m_unrestored[item["id"].toString()]=item;if(!m_recoveryOrder.contains(item["id"].toString()))m_recoveryOrder.append(item["id"].toString());}
    for(const auto &value:stored["drafts"].toArray()){const auto item=value.toObject();m_unrestored[item["id"].toString()]=item;if(!m_recoveryOrder.contains(item["id"].toString()))m_recoveryOrder.append(item["id"].toString());}
    for(auto it=m_unrestored.begin();it!=m_unrestored.end();)if(it->value("path").toString().isEmpty()&&!it->contains("content"))it=m_unrestored.erase(it);else ++it;
    m_recoveryTimer.setInterval(2000);connect(&m_recoveryTimer,&QTimer::timeout,this,&FileApplication::captureRecovery);if(!settingsPath.isEmpty())m_recoveryTimer.start();
    connect(m_recovery,&RecoveryStore::failed,this,[this](const QString &message){if(m_recoveryError==message)return;m_recoveryError=message;for(auto *w:m_workspaces)w->showNotice(message);for(auto *pane:m_windows)if(pane->isWindow())pane->reportHostMessage(message);});
    connect(m_recovery,&RecoveryStore::persisted,this,[this]{if(m_recoveryError.isEmpty())return;for(auto *workspace:m_workspaces)if(auto *notice=workspace->findChild<QLabel*>("workspaceNotice"))if(notice->text()==m_recoveryError)workspace->showNotice({});for(auto *pane:m_windows)pane->clearHostMessage(m_recoveryError);m_recoveryError.clear();});
    connect(&m_manager,&FileManager::documentsChanged,this,[this]{if(!m_destroying)QTimer::singleShot(0,this,&FileApplication::captureRecovery);});
    m_autoSave = new AutoSaveController(&m_manager, m_appearance.preferences(), this);
    connect(&m_manager, &FileManager::resourcesChanged, this, [this] {
        for (auto pane : m_documents) if (pane && pane->preview()) pane->preview()->refresh();
    });
    connect(&m_manager, &FileManager::opened, this, [this](QUuid intent, FileDocument *document, bool reused) {
        auto placeholder = m_requests.take(intent);
        const auto navigation = m_linkNavigations.take(intent);
        const bool workspaceRequested = m_workspaceRequests.contains(intent); const auto workspace = m_workspaceRequests.take(intent);
        if (navigation.root && !validLinkSource(navigation)) {
            if (!reused) m_manager.closeDocument(document);
            if (placeholder) { placeholder->finishOpen(); placeholder->close(); }
            if (navigation.source) navigation.source->reportHostMessage(QStringLiteral("链接来源或工作区已变化，本次打开已取消。"));
            return;
        }
        if (!placeholder) { if (!reused) m_manager.closeDocument(document); return; }
        if (workspaceRequested && (!workspace || workspace->closingWorkspace())) { if (!reused) m_manager.closeDocument(document); placeholder->finishOpen(); placeholder->close(); return; }
        if(!document->untitled())m_navigation.recordOpen(document->path());
        const auto id = document->session()->documentId(); auto pane = m_documents.value(id);
        if (pane) { if(m_restoreRequests.contains(intent)){const auto entry=m_restoreRequests.take(intent);m_unrestored.remove(entry["id"].toString());applyRestoredState(pane,entry);refreshRecoveryActions();} placeholder->finishOpen(); if (pane != placeholder) placeholder->close(); activatePane(pane); if (navigation.hasFragment) pane->navigateToHeading(navigation.fragment); if (navigation.sourceLocation) pane->navigateToReference(navigation.locationIdentity, navigation.sourceSha256, navigation.locationStart, navigation.locationEnd); return; }
        const bool embed = workspaceRequested && !document->untitled() && workspace->tree()->containsPath(document->path());
        if (embed && !workspace->addPane(placeholder)) { if (!reused) m_manager.closeDocument(document); placeholder->finishOpen(); placeholder->close(); workspace->showNotice(QStringLiteral("无法准备文档标签，原有文档保持不变。")); return; }
        m_documents.insert(id, placeholder); placeholder->setDocument(document);
        if (document->untitled() && !document->recoveredDraft()) placeholder->setViewMode(SingleFileWindow::ViewMode::Source);
        if(m_restoreRequests.contains(intent)){const auto entry=m_restoreRequests.take(intent);m_unrestored.remove(entry["id"].toString());applyRestoredState(placeholder,entry);refreshRecoveryActions();}
        connect(placeholder, &QObject::destroyed, this, [this, id] { m_documents.remove(id); });
        connect(document, &FileDocument::stateChanged, this, [this, document, path = document->path()]() mutable { if (path != document->path()) { path = document->path(); m_navigation.recordOpen(path); } QTimer::singleShot(0, this, &FileApplication::retryTransfers); });
        connect(document->session(), &DocumentSession::stateChanged, this, [this] { QTimer::singleShot(0, this, &FileApplication::retryTransfers); }); activatePane(placeholder);
        if (navigation.hasFragment) placeholder->navigateToHeading(navigation.fragment);
        if (navigation.sourceLocation) placeholder->navigateToReference(navigation.locationIdentity, navigation.sourceSha256, navigation.locationStart, navigation.locationEnd);
    });
    connect(&m_manager, &FileManager::openFailed, this, [this](QUuid intent, const QString &path, const QString &message) {
        m_restoreRequests.remove(intent);refreshRecoveryActions();
        auto pane = m_requests.take(intent); const auto navigation = m_linkNavigations.take(intent);
        const bool workspaceRequested = m_workspaceRequests.contains(intent); const auto workspace = m_workspaceRequests.take(intent);
        if (navigation.root) {
            if (pane) { pane->finishOpen(); pane->close(); }
            if (navigation.source) { navigation.source->reportHostMessage(QStringLiteral("无法打开文档链接：") + message); activatePane(navigation.source); }
        } else if (pane) {
            if (workspaceRequested && (!workspace || workspace->closingWorkspace())) { pane->finishOpen(); pane->close(); return; }
            if (workspaceRequested && workspace->tree()->containsPath(path) && !workspace->addPane(pane)) { pane->finishOpen(); pane->close(); return; }
            pane->showOpenError(path, message);
            if (!pane->embedded()) pane->show();
        }
    });
}
FileApplication::~FileApplication() {
    m_recoveryTimer.stop();captureRecovery();m_recovery->flush();
    m_destroying = true; qApp->removeEventFilter(this);
    m_autoSave->stop(); delete m_autoSave; m_autoSave = nullptr; if (m_settings) delete m_settings;
    QList<QPointer<WorkspaceWindow>> workspaces; for (auto *workspace : m_workspaces) workspaces.append(workspace);
    QList<QPointer<SingleFileWindow>> panes; for (auto *pane : m_windows) panes.append(pane);
    for (auto workspace : workspaces) if (workspace) delete workspace;
    for (auto pane : panes) if (pane) delete pane;
}
SingleFileWindow *FileApplication::createWindow() {
    auto *pane = new SingleFileWindow(&m_manager, &m_appearance, &m_geometryStore); pane->setAttribute(Qt::WA_DontShowOnScreen, m_presentation == Presentation::HiddenNative); m_windows.insert(pane);if(!m_recovery->error().isEmpty())pane->reportHostMessage(m_recovery->error());
    connect(pane, &SingleFileWindow::copyPathRequested, this, [](const QString &path) { QGuiApplication::clipboard()->setText(path); });
    connect(pane, &SingleFileWindow::copyTextRequested, this, [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
    connect(pane, &SingleFileWindow::revealRequested, this, [](const QString &path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    connect(pane, &SingleFileWindow::openRequested, this, [this, pane](const QString &path) { if (auto *workspace = qobject_cast<WorkspaceWindow *>(pane->window())) openInWorkspace(path, workspace); else open(path, pane->document() ? nullptr : pane); });
    connect(pane, &SingleFileWindow::standaloneOpenRequested, this, [this, pane](const QString &path) { open(path, !pane->embedded() && !pane->document() ? pane : nullptr); });
    connect(pane, &SingleFileWindow::workspaceRequested, this, [this, pane] { transferToWorkspace(pane); });
    connect(pane, &SingleFileWindow::settingsRequested, this, [this] { settingsWindow(); });
    connect(pane, &SingleFileWindow::openDirectoryRequested, this, [this, pane] { chooseDirectory(qobject_cast<WorkspaceWindow *>(pane->window())); });
    connect(pane, &SingleFileWindow::cancelOpenRequested, this, [this](QUuid intent) { m_requests.remove(intent); m_linkNavigations.remove(intent); m_workspaceRequests.remove(intent);m_restoreRequests.remove(intent);refreshRecoveryActions(); m_manager.cancelOpen(intent); });
    connect(pane, &SingleFileWindow::documentLinkRequested, this, [this, pane](const QString &target, quint64 revision) { openDocumentLink(pane, target, revision); });
    connect(pane, &SingleFileWindow::quitRequested, this, &FileApplication::requestQuit); connect(pane, &SingleFileWindow::closeCancelled, this, [this] { m_quitting = false; });
    connect(pane, &QObject::destroyed, this, [this, pane, id = pane->paneId()] { m_windows.remove(pane); m_transfers.remove(id); if (m_destroying) return; if (m_windows.isEmpty() && m_workspaces.isEmpty() && !m_settings) QCoreApplication::quit(); else if (m_quitting) QTimer::singleShot(0, this, &FileApplication::advanceQuit); }); return pane;
}
WorkspaceWindow *FileApplication::createWorkspace() {
    auto *workspace = new WorkspaceWindow(&m_appearance, &m_geometryStore, &m_navigation, &m_manager); workspace->setAttribute(Qt::WA_DontShowOnScreen, m_presentation == Presentation::HiddenNative); m_workspaces.insert(workspace);
    connect(workspace, &WorkspaceWindow::settingsRequested, this, [this] { settingsWindow(); });
    connect(workspace, &WorkspaceWindow::referenceSourceRequested, this, [this, workspace](const QString &path, const QString &identity, const QByteArray &sha, qsizetype start, qsizetype end) {
        LinkNavigation navigation; navigation.sourceLocation = true; navigation.workspace = workspace; navigation.root = workspace->resourceRoot();
        navigation.source = workspace->activePane(); navigation.sourceSha256 = sha; navigation.locationIdentity = identity; navigation.locationStart = start; navigation.locationEnd = end;
        if (!validLinkSource(navigation)) return;
        auto *placeholder = createWindow(); const auto intent = QUuid::createUuid(); placeholder->setLoading(QDir(navigation.root->rootPath()).filePath(path), intent);
        trackWorkspaceRequest(intent, placeholder, workspace);
        m_requests.insert(intent, placeholder); m_linkNavigations.insert(intent, navigation); m_manager.openWithin(navigation.root, path, intent);
    });
    connect(workspace, &WorkspaceWindow::openRequested, this, [this, workspace](const QString &path) { openInWorkspace(path, workspace); });
    connect(workspace, &WorkspaceWindow::chooseFileRequested, this, [this, workspace] { chooseFile(workspace, false); });
    connect(workspace, &WorkspaceWindow::chooseWorkspaceFileRequested, this, [this, workspace] { chooseFile(workspace, true); });
    connect(workspace, &WorkspaceWindow::newDocumentRequested, this, &FileApplication::newDocument);
    connect(workspace, &WorkspaceWindow::chooseDirectoryRequested, this, [this, workspace] { chooseDirectory(workspace); });
    connect(workspace, &WorkspaceWindow::closeCancelled, this, [this] { m_quitting = false; });
    connect(workspace, &WorkspaceWindow::quitRequested, this, &FileApplication::requestQuit);
    connect(workspace, &QObject::destroyed, this, [this, workspace] { m_workspaces.remove(workspace); if (m_destroying) return; if (m_windows.isEmpty() && m_workspaces.isEmpty() && !m_settings) QCoreApplication::quit(); else if (m_quitting) QTimer::singleShot(0, this, &FileApplication::advanceQuit); }); return workspace;
}
WorkspaceWindow *FileApplication::workspaceWindow(const QString &root) {
    for (auto *workspace : m_workspaces) if (!workspace->closingWorkspace() && ((root.isEmpty() && workspace->rootPath().isEmpty()) || (!root.isEmpty() && !workspace->rootPath().isEmpty() && rootKey(root) == rootKey(workspace->rootPath())))) { workspace->show(); workspace->raise(); workspace->activateWindow(); return workspace; }
    auto *workspace = createWorkspace(); workspace->setRootPath(root);
    if(!m_unrestored.isEmpty()) {auto *restore=new SharedButton(QStringLiteral("恢复上次会话（%1 项）").arg(m_unrestored.size()),"restoreLastSession",&m_appearance,workspace);restore->setOutlined(true);qobject_cast<QVBoxLayout*>(workspace->layout())->insertWidget(1,restore);connect(restore,&QPushButton::clicked,this,[this,restore]{restore->setEnabled(false);restoreSession();});}
    if(!m_recovery->error().isEmpty())workspace->showNotice(m_recovery->error());workspace->show(); return workspace;
}
void FileApplication::chooseDirectory(WorkspaceWindow *owner) {
    const auto root = QFileDialog::getExistingDirectory(owner, QStringLiteral("打开工作区目录"), owner ? owner->rootPath() : QString()); InputModality::forApplication()->clear(); if (root.isEmpty()) return;
    if (owner && owner->rootPath().isEmpty() && !owner->closingWorkspace()) owner->setRootPath(root); else workspaceWindow(root);
}
void FileApplication::chooseFile(WorkspaceWindow *owner, bool workspaceTab) {
    QPointer<WorkspaceWindow> workspace = owner;
    const auto path = QFileDialog::getOpenFileName(owner, QStringLiteral("打开 Markdown 文件"), owner->rootPath(), QStringLiteral("Markdown 文档 (*.md *.markdown);;所有文件 (*)"));
    InputModality::forApplication()->clear();
    if (path.isEmpty()) return;
    if (workspaceTab && workspace) openInWorkspace(path, workspace); else open(path);
}
SingleFileWindow *FileApplication::emptyWindow() { auto *pane = createWindow(); pane->show(); return pane; }
void FileApplication::newDocument() {
    m_quitting = false; auto *pane = createWindow(); const auto intent = QUuid::createUuid();
    m_requests.insert(intent, pane); pane->setLoading(QStringLiteral("未命名.md"), intent); m_manager.createUntitled(intent);
}
SettingsWindow *FileApplication::settingsWindow() {
    if (!m_settings) {
        m_settings = new SettingsWindow(&m_appearance, &m_geometryStore);
        m_settings->setAttribute(Qt::WA_DontShowOnScreen, m_presentation == Presentation::HiddenNative);
        connect(m_settings, &QObject::destroyed, this, [this] { if (!m_destroying && m_windows.isEmpty() && m_workspaces.isEmpty()) QCoreApplication::quit(); });
    }
    if (m_settings->isMinimized()) m_settings->showNormal(); m_settings->show(); m_settings->raise(); m_settings->activateWindow(); return m_settings;
}
void FileApplication::openExternal(const QString &path) {
    if (m_appearance.preferences()->value("desktop", "externalOpenTarget").toString() != "workspace-tab") { open(path); return; }
    auto *target = qobject_cast<WorkspaceWindow *>(QApplication::activeWindow());
    if (!m_workspaces.contains(target) || target->closingWorkspace() || !target->tree()->containsPath(path)) target = nullptr;
    if (!target) for (auto *workspace : m_workspaces) if (!workspace->closingWorkspace() && workspace->tree()->containsPath(path)) { target = workspace; break; }
    if (target) openInWorkspace(path, target); else open(path);
}
void FileApplication::open(const QString &path, SingleFileWindow *empty) {
    m_quitting = false; if (empty && empty->embedded()) { openInWorkspace(path, qobject_cast<WorkspaceWindow *>(empty->window())); return; }
    auto *pane = empty && !empty->document() ? empty : createWindow();
    for (auto it = m_requests.begin(); it != m_requests.end();) { if (it.value() == pane) { m_restoreRequests.remove(it.key());refreshRecoveryActions();m_manager.cancelOpen(it.key()); it = m_requests.erase(it); } else ++it; }
    const auto intent = QUuid::createUuid(); m_requests.insert(intent, pane); pane->setLoading(path, intent); pane->show(); m_manager.open(path, intent);
}
void FileApplication::openInWorkspace(const QString &path, WorkspaceWindow *workspace) {
    if (!workspace || workspace->closingWorkspace()) return;
    if (!workspace->tree()->containsPath(path)) { open(path); return; }
    m_quitting = false;
    auto *pane = createWindow(); const auto intent = QUuid::createUuid(); pane->setLoading(path, intent);
    trackWorkspaceRequest(intent, pane, workspace);
    m_requests.insert(intent, pane); m_manager.open(path, intent);
}
void FileApplication::activatePane(SingleFileWindow *pane) { if (auto *workspace = qobject_cast<WorkspaceWindow *>(pane->window())) workspace->activatePane(pane->paneId()); else { if (pane->isMinimized()) pane->showNormal(); pane->show(); pane->raise(); pane->activateWindow(); } }
void FileApplication::trackWorkspaceRequest(const QUuid &intent, SingleFileWindow *pane, WorkspaceWindow *workspace) {
    m_workspaceRequests.insert(intent, workspace);
    connect(workspace, &QObject::destroyed, pane, [this, pane, intent] {
        // The unshown request belongs to its destination, even before it is a tab.
        if (!m_workspaceRequests.remove(intent)) return;
        m_requests.remove(intent); m_linkNavigations.remove(intent);m_restoreRequests.remove(intent);refreshRecoveryActions(); m_manager.cancelOpen(intent); pane->finishOpen(); pane->close();
    });
}
bool FileApplication::validLinkSource(const LinkNavigation &navigation) const {
    if (navigation.sourceLocation) return navigation.workspace && !navigation.workspace->closingWorkspace()
        && navigation.root && navigation.root->active() && navigation.workspace->resourceRoot() == navigation.root;
    const auto source = navigation.source;
    if (!source || !navigation.root || !navigation.root->active() || !source->document() || source->document()->closed()
        || source->resourceRoot() != navigation.root || source->resourceDocumentPath() != navigation.sourcePath
        || source->document()->identity() != navigation.sourceIdentity || source->embedded() != navigation.embedded) return false;
    if (navigation.embedded && (!navigation.workspace || navigation.workspace->closingWorkspace()
        || source->window() != navigation.workspace || navigation.workspace->resourceRoot() != navigation.root)) return false;
    const auto snapshot = source->document()->session()->snapshot();
    return snapshot.result == EditResult::Applied && snapshot.revision == navigation.revision;
}
void FileApplication::openDocumentLink(SingleFileWindow *source, const QString &target, quint64 revision) {
    if (!source || !source->document() || source->document()->closed()) return;
    LinkNavigation navigation; navigation.source = source; navigation.workspace = qobject_cast<WorkspaceWindow *>(source->window());
    navigation.root = source->resourceRoot(); navigation.sourcePath = source->resourceDocumentPath();
    navigation.sourceIdentity = source->document()->identity(); navigation.revision = revision; navigation.embedded = source->embedded();
    if (!navigation.root) {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        source->reportHostMessage(QStringLiteral("此平台暂不支持受控跨文档链接打开。"));
#else
        source->reportHostMessage(QStringLiteral("此文档没有可用的目录授权，请先打开所属工作区或重新打开文档。"));
#endif
        return;
    }
    if (!validLinkSource(navigation)) { source->reportHostMessage(QStringLiteral("文档或目录授权不可用，请完成输入或重新打开目录后重试。")); return; }
    const auto resolved = resolveWorkspaceTarget(navigation.sourcePath, target);
    if (!resolved.success) { source->reportHostMessage(QStringLiteral("此文档链接超出当前授权范围，或不是支持的相对路径。")); return; }
    const auto suffix = QFileInfo(resolved.path).suffix().toLower();
    if (suffix != "md" && suffix != "markdown") { source->reportHostMessage(QStringLiteral("此入口仅打开 Markdown 文档，不执行其他文件。")); return; }
    navigation.fragment = resolved.fragment; navigation.hasFragment = resolved.hasFragment && !resolved.fragment.isEmpty();
    m_quitting = false;
    auto *placeholder = createWindow(); const auto intent = QUuid::createUuid();
    placeholder->setLoading(QDir(navigation.root->rootPath()).filePath(resolved.path), intent);
    if (navigation.workspace) {
        trackWorkspaceRequest(intent, placeholder, navigation.workspace);
    } else placeholder->show();
    m_requests.insert(intent, placeholder); m_linkNavigations.insert(intent, navigation);
    m_manager.openWithin(navigation.root, resolved.path, intent, navigation.sourcePath, navigation.sourceIdentity);
}
bool FileApplication::transferToWorkspace(SingleFileWindow *pane, WorkspaceWindow *target) {
    if (!pane || !pane->document() || pane->document()->closed()) return false;
    if (pane->embedded()) { activatePane(pane); return true; }
    if (target && target->closingWorkspace()) { m_transfers.remove(pane->paneId()); pane->reportHostMessage(QStringLiteral("工作区正在关闭，文档仍保留在独立窗口。")); return false; }
    if (!target) {
        auto *active = qobject_cast<WorkspaceWindow *>(QApplication::activeWindow());
        if (m_workspaces.contains(active) && !active->closingWorkspace() && active->tree()->acceptsFile(pane->document()->path())) target = active;
        if (!target) for (auto *workspace : m_workspaces) if (!workspace->closingWorkspace() && workspace->tree()->acceptsFile(pane->document()->path())) { target = workspace; break; }
    }
    if (!target || pane->document()->untitled() || !target->tree()->acceptsFile(pane->document()->path())) {
        m_transfers.remove(pane->paneId());
        pane->reportHostMessage(QStringLiteral("请先打开包含此文件的工作区目录；目录外文件保留在独立窗口。"));
        return false;
    }
    if (!pane->canTransfer()) {
        const auto *document = pane->document();
        const bool pending = document->saving() || document->loading() || document->session()->isBusy() || document->session()->isComposing() || (pane->editor() && pane->editor()->dispatching());
        if (pending) { m_transfers.insert(pane->paneId(), {pane, target, target != nullptr}); pane->reportHostMessage(QStringLiteral("当前输入或文件操作结束后，将在工作区继续处理。")); }
        else pane->reportHostMessage(QStringLiteral("请先完成当前对话框，再选择在工作区打开。"));
        return false;
    }
    m_transfers.remove(pane->paneId());
    if (!target->addPane(pane)) { pane->reportHostMessage(QStringLiteral("工作区未能接管文档，独立窗口与编辑内容已保留。")); return false; }
    pane->reportHostMessage({}); return true;
}
void FileApplication::retryTransfers() {
    const auto transfers = m_transfers;
    for (auto it = transfers.begin(); it != transfers.end(); ++it) { const auto pending = it.value(); if (!pending.pane) { m_transfers.remove(it.key()); continue; } if (pending.explicitTarget && !pending.target) { m_transfers.remove(it.key()); pending.pane->reportHostMessage(QStringLiteral("目标工作区已关闭，文档仍保留在独立窗口。")); } else if (pending.pane->canTransfer()) transferToWorkspace(pending.pane, pending.target); }
}
void FileApplication::activate() { for (auto *workspace : m_workspaces) { if (workspace->isMinimized()) workspace->showNormal(); workspace->raise(); workspace->activateWindow(); return; } if (!m_windows.isEmpty()) activatePane(*m_windows.begin()); else workspaceWindow(); }
void FileApplication::requestQuit() { if (!m_quitting) { captureRecovery(); m_quitting = true; advanceQuit(); } }
void FileApplication::advanceQuit() { if (!m_quitting) return; for (auto *workspace : m_workspaces) if (workspace->isVisible()) { workspace->close(); return; } for (auto *pane : m_windows) if (pane->isWindow() && pane->isVisible()) { pane->close(); return; } if (m_windows.isEmpty() && m_workspaces.isEmpty()) { if (m_settings) m_settings->close(); else QCoreApplication::quit(); } }
bool FileApplication::eventFilter(QObject *target, QEvent *event) {
    if(event->type()==QEvent::Close && !m_destroying && (m_windows.contains(qobject_cast<SingleFileWindow*>(target))||m_workspaces.contains(qobject_cast<WorkspaceWindow*>(target))))captureRecovery();
    if (target == qApp && event->type() == QEvent::FileOpen) { const auto *file = static_cast<QFileOpenEvent *>(event); if (!file->file().isEmpty()) openExternal(file->file()); else if (file->url().isLocalFile()) openExternal(file->url().toLocalFile()); return true; }
    if (target == qApp && event->type() == QEvent::Quit && (!m_windows.isEmpty() || !m_workspaces.isEmpty())) { requestQuit(); return true; }
    return QObject::eventFilter(target, event);
}
void FileApplication::captureRecovery(){
    if(!m_recovery||m_destroying)return;
    QJsonArray current,drafts;QSet<QString> live;
    QList<QPointer<SingleFileWindow>> ordered;for(auto *workspace:m_workspaces)for(auto *pane:workspace->panes())ordered.append(pane);for(auto *pane:m_windows)if(pane->isWindow())ordered.append(pane);
    for(auto pane:ordered){if(!pane||!pane->document()||pane->document()->closed())continue;QPointer<FileDocument> doc=pane->document();const auto id=doc->session()->documentId().toString(QUuid::WithoutBraces);live.insert(id);const auto snap=doc->session()->snapshot();if(!pane||!doc||doc->closed())continue;
        if(snap.result!=EditResult::Applied){if(m_liveRecovery.contains(id)){auto item=m_liveRecovery[id];current.append(item);if(item.contains("content"))drafts.append(item);}continue;}
        auto *workspace=qobject_cast<WorkspaceWindow*>(pane->window());QJsonObject item{{"id",id},{"path",doc->untitled()?QString():doc->path()},{"root",workspace?workspace->rootPath():QString()},{"mode",int(pane->viewMode())},{"zoom",pane->contentZoom()},{"bom",doc->bom()}};
        if(auto *editor=pane->editor()){item["cursor"]=int(editor->send(SCI_GETCURRENTPOS));item["anchor"]=int(editor->send(SCI_GETANCHOR));item["line"]=int(editor->send(SCI_GETFIRSTVISIBLELINE));}
        if(pane->preview())item["readerY"]=pane->preview()->readingPosition().y();
        if(doc->dirty()||doc->untitled())item["content"]=QString::fromLatin1(snap.utf8.toBase64());m_liveRecovery[id]=item;current.append(item);if(item.contains("content"))drafts.append(item);
    }
    for(auto it=m_liveRecovery.begin();it!=m_liveRecovery.end();)if(!live.contains(it.key()))it=m_liveRecovery.erase(it);else ++it;
    for(const auto &item:m_unrestored){if(item.contains("content"))drafts.append(item);current.append(item);}
    if(!current.isEmpty()&&!m_quitting){m_lastSession={};for(const auto &value:current){auto item=value.toObject();item.remove("content");m_lastSession.append(item);}}
    QSet<QString> draftIds;for(const auto &value:drafts)draftIds.insert(value.toObject()["id"].toString());QJsonArray retained;for(const auto &value:m_lastSession){const auto entry=value.toObject();if(!entry["path"].toString().isEmpty()||draftIds.contains(entry["id"].toString()))retained.append(value);}m_lastSession=retained;
    m_recovery->save({{"schemaVersion",1},{"documents",m_lastSession},{"drafts",drafts}});
}
void FileApplication::applyRestoredState(SingleFileWindow *pane,const QJsonObject &entry){
    const int mode=entry["mode"].toInt();pane->setViewMode(mode==2&&pane->embedded()?SingleFileWindow::ViewMode::Split:mode==1?SingleFileWindow::ViewMode::Source:SingleFileWindow::ViewMode::Reading);pane->setContentZoom(qBound(50,entry["zoom"].toInt(100),200));
    if(auto *editor=pane->editor()){const auto snap=pane->document()->session()->snapshot();const auto clamp=[&](int at){at=qBound(0,at,int(snap.utf8.size()));while(at>0&&at<snap.utf8.size()&&(quint8(snap.utf8[at])&0xc0)==0x80)--at;return at;};editor->send(SCI_SETSEL,uptr_t(clamp(entry["anchor"].toInt())),sptr_t(clamp(entry["cursor"].toInt())));editor->send(SCI_SETFIRSTVISIBLELINE,uptr_t(qMax(0,entry["line"].toInt())));}
    if(pane->preview())pane->preview()->restoreReadingPosition(QPointF(0,qMax(0.0,entry["readerY"].toDouble())));
    if(entry.contains("content"))pane->reportHostMessage(QStringLiteral("已恢复未保存内容的副本；原文件保持不变，请另存为。原位置：")+entry["path"].toString());
}
void FileApplication::restoreSession(){
    if(m_restoring)return;m_restoring=true;const auto entries=m_unrestored;
    for(const auto &id:m_recoveryOrder){if(!entries.contains(id))continue;const auto entry=entries.value(id);auto *workspace=entry["root"].toString().isEmpty()?nullptr:workspaceWindow(entry["root"].toString());auto *pane=createWindow();const auto intent=QUuid::createUuid();m_requests[intent]=pane;m_restoreRequests[intent]=entry;pane->setLoading(entry["path"].toString(),intent);if(workspace&&!entry.contains("content")&&workspace->tree()->containsPath(entry["path"].toString()))trackWorkspaceRequest(intent,pane,workspace);else pane->show();
        if(entry.contains("content"))m_manager.createDraft(QByteArray::fromBase64(entry["content"].toString().toLatin1()),intent,entry["bom"].toBool());else if(!entry["path"].toString().isEmpty())m_manager.open(entry["path"].toString(),intent);else{m_requests.remove(intent);m_restoreRequests.remove(intent);pane->finishOpen();pane->close();}
    }
    refreshRecoveryActions();
}
void FileApplication::refreshRecoveryActions(){
    QTimer::singleShot(0,this,[this]{if(m_destroying)return;m_restoring=!m_restoreRequests.isEmpty();for(auto *workspace:m_workspaces)if(auto *button=workspace->findChild<SharedButton*>("restoreLastSession")){button->setEnabled(!m_restoring&&!m_unrestored.isEmpty());button->setVisible(m_restoring||!m_unrestored.isEmpty());button->setText(m_restoring?QStringLiteral("正在恢复会话…"):QStringLiteral("恢复上次会话（%1 项）").arg(m_unrestored.size()));}});
}

}
