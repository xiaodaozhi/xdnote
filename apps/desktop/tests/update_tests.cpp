#include <QScopeGuard>
#include "UpdateDownload.h"
#include "UpdateReceipt.h"
#include "RecoveryStore.h"
#include "FileApplication.h"
#include "SingleFileWindow.h"
#include "SourceEditor.h"
#include "UpdateTaskGate.h"
#include <QCryptographicHash>
#include <QFile>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <atomic>
#include "UpdateController.h"
#include "UpdateNetwork.h"
#include "UpdateBridge.h"
#include "BuildInfo.h"
#include "DocumentSession.h"
#include "SettingsWindow.h"
#include "Appearance.h"
#include "SharedWidgets.h"
#include "GuiRuntime.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxyFactory>
#include <QSslError>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QDir>
#include <QScrollArea>
#include <QStackedWidget>
#include <cstring>
using namespace xdnote;
namespace {
struct Response {
    QByteArray bytes = "[]";
    int status = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QList<QPair<QByteArray,QByteArray>> headers;
    QUrl redirect;
    bool hold = false, tls = false, finalOnly = false;
};
class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest &request, Response response, QObject *parent) : QNetworkReply(parent), response(std::move(response)) {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute,this->response.status);
        for (const auto &header : this->response.headers) setRawHeader(header.first,header.second);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0,this,[this] {
            if (this->response.hold) return;
            if (this->response.tls) emit sslErrors({QSslError(QSslError::SelfSignedCertificate)});
            if (isFinished()) return;
            if (!this->response.redirect.isEmpty()) emit redirected(this->response.redirect);
            if (isFinished()) return;
            if (!this->response.finalOnly) emit readyRead();
            if (isFinished()) return;
            if (this->response.error != NoError) setError(this->response.error,"fixture network failure");
            setFinished(true); emit finished();
        });
    }
    void abort() override { if (!isFinished()) { setError(OperationCanceledError,"fixture abort");setFinished(true);emit finished(); } }
    qint64 bytesAvailable() const override { return response.bytes.size()-offset+QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data,qint64 maximum) override {
        const auto count=qMin(maximum,qint64(response.bytes.size()-offset));
        if (count<=0) return -1;
        std::memcpy(data,response.bytes.constData()+offset,std::size_t(count));offset+=count;return count;
    }
private:
    Response response;
    qint64 offset=0;
};
class Network final : public QNetworkAccessManager {
public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    bool exhausted = false;
protected:
    QNetworkReply *createRequest(Operation op,const QNetworkRequest &request,QIODevice *data) override {
        Q_UNUSED(op);Q_UNUSED(data);requests.append(request);
        if (responses.isEmpty()) { exhausted=true; Response error;error.error=QNetworkReply::ConnectionRefusedError;return new Reply(request,error,this); }
        return new Reply(request,responses.takeFirst(),this);
    }
};
QJsonObject release(const QString &version="0.2.0") {
    const auto root="https://github.com/xiaodaozhi/xdnote/releases/download/v"+version+"/";
    return {{"tag_name","v"+version},{"draft",false},{"assets",QJsonArray{
        QJsonObject{{"name","release.json"},{"browser_download_url",root+"release.json"}},
        QJsonObject{{"name","release.json.sig"},{"browser_download_url",root+"release.json.sig"}},
        QJsonObject{{"name","xdnote-"+version+"-windows-x86_64.zip"},{"browser_download_url",root+"xdnote-"+version+"-windows-x86_64.zip"},{"size",12}}
    }}};
}
QByteArray manifest(const QString &version="0.2.0") {
    QJsonArray assets;
    const QList<QStringList> rows{
        {"windows","x86_64","zip","windows-x86_64.zip"},{"windows","x86_64","exe","windows-x86_64-setup.exe"},
        {"macos","arm64","app.zip","macos-arm64.app.zip"},{"macos","arm64","dmg","macos-arm64.dmg"},
        {"linux","x86_64","tar.gz","linux-x86_64.tar.gz"},{"linux","x86_64","deb","ubuntu22.04-x86_64.deb"}};
    for (const auto &row:rows) {
        const auto filename="xdnote-"+version+"-"+row[3];
        const QJsonObject minimum=row[0]=="windows"?QJsonObject{{"windows","10.0.17763"}}:row[0]=="macos"?QJsonObject{{"macos","14.0"}}:QJsonObject{{"distribution","ubuntu"},{"version","22.04"},{"glibc","2.35"},{"display_server","Wayland/X11"}};
        assets.append(QJsonObject{{"filename",filename},{"platform",row[0]},{"architecture",row[1]},{"package_type",row[2]},
            {"minimum_system",minimum},{"bytes",12},{"sha256",QString(64,'a')},{"url","https://github.com/xiaodaozhi/xdnote/releases/download/v"+version+"/"+filename}});
    }
    return QJsonDocument(QJsonObject{{"schema_version",1},{"product","xdnote"},{"version",version},{"channel","stable"},
        {"build",10},{"tag","v"+version},{"commit",QString(40,'b')},{"repository","xiaodaozhi/xdnote"},{"assets",assets}}).toJson();
}
QByteArray listing(const QJsonArray &values) { return QJsonDocument(values).toJson(); }
const QString current="0.1.0-dev.123456abcdef";
}
class UpdateTests final : public QObject {
    Q_OBJECT
private slots:
    void receiptsFollowVersionedDebianImagesWithoutAcceptingOtherInstallations() {
        const QString oldRoot = "/opt/xdnote/versions/old", newRoot = "/opt/xdnote/versions/new";
        QVERIFY(updateReceiptPaths("linux", oldRoot, oldRoot + "/bin/xdnote", newRoot, newRoot + "/bin/xdnote"));
        for (const QString &root : {QString("/tmp/xdnote/versions/old"), QString("/opt/xdnote/versions/../old"), QString("/opt/xdnote/versions/old/nested")})
            QVERIFY(!updateReceiptPaths("linux", root, root + "/bin/xdnote", newRoot, newRoot + "/bin/xdnote"));
        QVERIFY(!updateReceiptPaths("linux", oldRoot, "/usr/bin/xdnote", newRoot, newRoot + "/bin/xdnote"));
        QVERIFY(!updateReceiptPaths("macos", oldRoot, oldRoot + "/xdnote", newRoot, newRoot + "/xdnote"));
        QVERIFY(updateReceiptPaths("windows", "C:/owned", "C:/owned/xdnote.exe", "C:/owned", "C:/owned/xdnote.exe"));
        QVERIFY(!updateReceiptPaths("windows", "C:/owned", "C:/owned/xdnote.exe", "C:/other", "C:/other/xdnote.exe"));
    }
    void authenticatedDownloadMetersFinalBytesAndRemovesFailures() {
        const QByteArray bytes(160 * 1024, 'd');
        const QJsonObject candidate{{"asset_url", "https://github.com/xiaodaozhi/xdnote/releases/download/v0.2.0/fixture.zip"}, {"filename", "fixture.zip"}, {"bytes", bytes.size()}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        QTemporaryDir cache;
        for (const auto &mode : {QString("valid"), QString("truncated"), QString("extra"), QString("hash"), QString("tls"), QString("redirect"), QString("http")}) {
            Network network; Response response; response.bytes = bytes; response.finalOnly = true;
            if (mode == "truncated") response.bytes.chop(1);
            if (mode == "extra") response.bytes += 'x';
            if (mode == "hash") response.bytes[0] = 'x';
            if (mode == "tls") response.tls = true;
            if (mode == "redirect") response.redirect = QUrl("https://example.invalid/package");
            if (mode == "http") response.status = 403;
            network.responses = {response}; UpdateDownload download(&network); QSignalSpy done(&download, &UpdateDownload::finished);
            download.start(candidate, {}, cache.path()); QTRY_COMPARE(done.size(), 1);
            const auto error = done.first().first().toString();
            if (mode == "valid") { QVERIFY2(error.isEmpty(), qPrintable(error)); QFile file(download.packagePath()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); }
            else { QVERIFY2(!error.isEmpty(), qPrintable(mode)); QVERIFY(download.packagePath().isEmpty()); }
        }
        QCOMPARE(QDir(cache.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void cancellationRemovesPartialPackageAndDoesNotAcceptStaleReply() {
        Network network; Response held; held.hold = true; held.bytes = "partial"; network.responses = {held};
        QTemporaryDir cache; UpdateDownload download(&network); QSignalSpy done(&download, &UpdateDownload::finished);
        download.start({{"asset_url", "https://github.com/xiaodaozhi/xdnote/releases/download/v0.2.0/fixture.zip"}, {"filename", "fixture.zip"}, {"bytes", 7}, {"sha256", QString(64, 'a')}}, {}, cache.path());
        QVERIFY(download.active()); download.cancel(); QTest::qWait(10); QVERIFY(!download.active()); QVERIFY(download.packagePath().isEmpty()); QCOMPARE(done.size(), 0);
        QCOMPARE(QDir(cache.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
    void helperLeasePreservesVerifiedTransactionAcrossUiCancellationAndRetry() {
        Network network; Response response; response.bytes = "owned final package"; response.finalOnly = true;
        network.responses = {response, response}; QTemporaryDir cache;
        UpdateDownload download(&network); QSignalSpy done(&download, &UpdateDownload::finished);
        const QJsonObject candidate{{"asset_url", "https://github.com/xiaodaozhi/xdnote/releases/download/v0.2.0/fixture.zip"},
            {"filename", "fixture.zip"}, {"bytes", response.bytes.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(response.bytes, QCryptographicHash::Sha256).toHex())}};
        download.start(candidate, {}, cache.path()); QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.first().first().toString().isEmpty()); const auto leased = download.directory();
        download.retainForHelper(); download.cancel();
        QVERIFY(download.packagePath().isEmpty()); QVERIFY(QFileInfo::exists(leased + "/fixture.zip"));
        QFile marker(leased + "/cancel.json"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("owned cancellation"); marker.close();
        download.start(candidate, {}, cache.path()); QTRY_COMPARE(done.size(), 2);
        QVERIFY(download.directory() != leased); QVERIFY(marker.open(QIODevice::ReadOnly)); QCOMPARE(marker.readAll(), QByteArray("owned cancellation")); marker.close();
        download.cancel(); QVERIFY(QFileInfo::exists(leased));
        QVERIFY(QDir(leased).removeRecursively());
    }
    void cancellingSecondDocumentKeepsEveryWindowAndUndoBuffer() {
        QTemporaryDir directory;
        FileApplication app(nullptr, directory.filePath("settings.json"), FileApplication::Presentation::HiddenNative);
        QStringList paths{directory.filePath("first.md"), directory.filePath("second.md")};
        for (const auto &path : paths) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("owned"); file.close(); app.open(path); }
        QList<QPointer<SingleFileWindow>> panes;
        QTRY_VERIFY(([&] { panes.clear(); for (auto widget : QApplication::topLevelWidgets()) if (auto pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && paths.contains(pane->document()->path())) panes.append(pane); return panes.size() == 2; })());
        for (auto pane : panes) QCOMPARE(pane->document()->session()->replace(0, 0, "dirty ", pane->document()->session()->revision()), EditResult::Applied);
        int choices = 0; QTimer answer; answer.setInterval(10);
        connect(&answer, &QTimer::timeout, qApp, [&] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") if (auto button = widget->findChild<SharedButton *>(choices++ == 0 ? "choice2" : "choice0")) button->click(); }); answer.start();
        bool finished = false, ready = true;
        app.prepareForUpdate([&](bool value) { finished = true; ready = value; }); QTRY_VERIFY(finished); answer.stop();
        QVERIFY(!ready); QVERIFY(!updateTasksBlocked()); QVERIFY(!app.updateHandoffActive());
        for (auto pane : panes) { QVERIFY(pane); QVERIFY(pane->isEnabled()); auto session = pane->document()->session(); QCOMPARE(session->snapshot().utf8, QByteArray("dirty owned")); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("owned")); }
    }
    void emptyProductionRootCannotDownloadOrInstall() {
        Network network; network.responses = {Response{listing({release()})}, Response{manifest()}, Response{QByteArray("{\"schema_version\":1,\"algorithm\":\"ed25519\",\"key_id\":\"test\",\"signature\":\"") + QByteArray(128, '0') + "\"}"}};
        UpdateController updates(&network, current, "windows", "x86_64", false);
        updates.check(); QTRY_VERIFY(!updates.busy()); QVERIFY(!updates.canDownload()); QVERIFY(!updates.canInstall());
        const auto count = network.requests.size(); updates.download(); updates.install(); QCOMPARE(network.requests.size(), count);
    }
    void explicitUpdateDiscardRetiresRecoveryOnlyForTheSealedHandoff() {
        for (const bool cancel : {false, true}) {
            QTemporaryDir directory; const auto path = directory.filePath("owned.md"), settings = directory.filePath("settings.json");
            { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("owned"); }
            const QJsonObject unrelated{{"id", "11111111-1111-4111-8111-111111111111"}, {"path", directory.filePath("other.md")}, {"root", ""}, {"content", QString::fromLatin1(QByteArray("owned unrelated recovery\n").toBase64())}};
            { RecoveryStore seed(settings + ".recovery"); seed.save({{"schemaVersion", 1}, {"documents", QJsonArray{}}, {"drafts", QJsonArray{unrelated}}}); QVERIFY(seed.flush()); }
            {
                FileApplication app(nullptr, settings, FileApplication::Presentation::HiddenNative); app.open(path);
                QPointer<SingleFileWindow> pane;
                QTRY_VERIFY(([&] { for (auto widget : QApplication::topLevelWidgets()) if (auto p = qobject_cast<SingleFileWindow *>(widget)) if (p->document() && p->document()->path() == path && !p->document()->loading()) pane = p; return bool(pane); })());
                auto session = pane->document()->session(); QCOMPARE(session->replace(0, 0, "edited ", session->revision()), EditResult::Applied);
                QTimer answer; answer.setInterval(10); connect(&answer, &QTimer::timeout, qApp, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") if (auto button = widget->findChild<SharedButton *>("choice2")) button->click(); }); answer.start();
                bool done = false, ready = false; app.prepareForUpdate([&](bool value) { ready = value; done = true; }); QTRY_VERIFY(done); answer.stop(); QVERIFY(ready);
                { RecoveryStore before(settings + ".recovery"); QCOMPARE(before.state()["drafts"].toArray().size(), 2); }
                QVERIFY(app.sealUpdateRecovery());
                { RecoveryStore sealed(settings + ".recovery"); QCOMPARE(sealed.state()["drafts"].toArray(), QJsonArray{unrelated}); }
                QCOMPARE(session->snapshot().utf8, QByteArray("edited owned"));
                if (cancel) {
                    app.cancelUpdateHandoff(); QVERIFY(!updateTasksBlocked()); QVERIFY(pane->isEnabled());
                    { RecoveryStore restored(settings + ".recovery"); QCOMPARE(restored.state()["drafts"].toArray().size(), 2); }
                    QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->redo(), EditResult::Applied);
                    QCOMPARE(session->snapshot().utf8, QByteArray("edited owned"));
                }
            }
            RecoveryStore final(settings + ".recovery"); QCOMPARE(final.state()["drafts"].toArray().size(), cancel ? 2 : 1);
            QFile disk(path); QVERIFY(disk.open(QIODevice::ReadOnly)); QCOMPARE(disk.readAll(), QByteArray("owned"));
        }
    }
    void laterDocumentPromptCannotCommitAnEarlierStaleDiscard() {
        QTemporaryDir directory; FileApplication app(nullptr, directory.filePath("settings.json"), FileApplication::Presentation::HiddenNative);
        const QStringList paths{directory.filePath("first.md"), directory.filePath("second.md")};
        for (const auto &path : paths) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("owned"); file.close(); app.open(path); }
        QList<QPointer<SingleFileWindow>> panes;
        QTRY_VERIFY(([&] { panes.clear(); for (auto widget : QApplication::topLevelWidgets()) if (auto pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && paths.contains(pane->document()->path()) && !pane->document()->loading()) panes.append(pane); return panes.size() == 2; })());
        for (auto pane : panes) QCOMPARE(pane->document()->session()->replace(0, 0, "dirty ", pane->document()->session()->revision()), EditResult::Applied);
        int prompts = 0; bool changed = false; QPointer<SingleFileWindow> first; QTimer answer; answer.setInterval(10);
        connect(&answer, &QTimer::timeout, qApp, [&] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") {
            const int index = prompts++;
            if (!index) first = qobject_cast<SingleFileWindow *>(widget->parentWidget());
            if (index == 1 && first) { auto session = first->document()->session(); changed = session->replace(0, 0, "late ", session->revision()) == EditResult::Applied; }
            if (auto button = widget->findChild<SharedButton *>(index < 2 ? "choice2" : "choice0")) button->click();
        }}); answer.start();
        bool finished = false, ready = true; app.prepareForUpdate([&](bool value) { ready = value; finished = true; }); QTRY_VERIFY(finished); answer.stop();
        QVERIFY(changed); QCOMPARE(prompts, 3); QVERIFY(!ready); QVERIFY(!updateTasksBlocked()); QVERIFY(first);
        QCOMPARE(first->document()->session()->snapshot().utf8, QByteArray("late dirty owned"));
        QCOMPARE(first->document()->session()->undo(), EditResult::Applied); QCOMPARE(first->document()->session()->snapshot().utf8, QByteArray("dirty owned"));
    }
    void cancelledHandoffCannotAcceptALateAnswerFromItsDialog() {
        QTemporaryDir directory; const auto path = directory.filePath("owned.md");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("owned"); }
        FileApplication app(nullptr, directory.filePath("settings.json"), FileApplication::Presentation::HiddenNative); app.open(path);
        QPointer<SingleFileWindow> pane;
        QTRY_VERIFY(([&] { for (auto widget : QApplication::topLevelWidgets()) if (auto p = qobject_cast<SingleFileWindow *>(widget)) if (p->document() && p->document()->path() == path && !p->document()->loading()) pane = p; return bool(pane); })());
        auto session = pane->document()->session(); QCOMPARE(session->replace(0, 0, "dirty ", session->revision()), EditResult::Applied);
        int prompts = 0; QTimer answer; answer.setInterval(10);
        connect(&answer, &QTimer::timeout, qApp, [&] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") {
            const int index = prompts++;
            if (!index) app.cancelUpdateHandoff();
            if (auto button = widget->findChild<SharedButton *>(!index ? "choice2" : "choice0")) button->click();
        }}); answer.start();
        for (int i = 0; i < 2; ++i) { bool done = false, ready = true; app.prepareForUpdate([&](bool value) { ready = value; done = true; }); QTRY_VERIFY(done); QVERIFY(!ready); QVERIFY(!updateTasksBlocked()); }
        answer.stop(); QCOMPARE(prompts, 2); QCOMPARE(session->snapshot().utf8, QByteArray("dirty owned")); QCOMPARE(session->undo(), EditResult::Applied);
    }
    void cancellingFrozenHandoffIgnoresWindowsDestroyedDuringTheWait() {
        QTemporaryDir directory; FileApplication app(nullptr, directory.filePath("settings.json"), FileApplication::Presentation::HiddenNative);
        const QStringList paths{directory.filePath("first.md"), directory.filePath("second.md")};
        for (const auto &path : paths) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("owned"); file.close(); app.open(path); }
        QList<QPointer<SingleFileWindow>> panes;
        QTRY_VERIFY(([&] { panes.clear(); for (auto widget : QApplication::topLevelWidgets()) if (auto pane = qobject_cast<SingleFileWindow *>(widget)) if (pane->document() && paths.contains(pane->document()->path()) && !pane->document()->loading()) panes.append(pane); return panes.size() == 2; })());
        bool done = false, ready = false; app.prepareForUpdate([&](bool value) { ready = value; done = true; }); QTRY_VERIFY(done); QVERIFY(ready); QVERIFY(updateTasksBlocked());
        delete panes[0].data(); QVERIFY(!panes[0]); app.cancelUpdateHandoff();
        QVERIFY(!updateTasksBlocked()); QVERIFY(panes[1]); QVERIFY(panes[1]->isEnabled()); QVERIFY(!panes[1]->document()->session()->isReadOnly());
        QCOMPARE(panes[1]->document()->session()->snapshot().utf8, QByteArray("owned"));
    }
    void documentBarrierJoinsWorkAndRestoresSameSessionOnCancellation() {
        QTemporaryDir directory; const auto path = directory.filePath("owned.md"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("# owned\n"); file.close();
        FileApplication app(nullptr, directory.filePath("settings.json"), FileApplication::Presentation::HiddenNative); app.open(path);
        SingleFileWindow *pane = nullptr;
        QTRY_VERIFY(([&] { for (auto widget : QApplication::topLevelWidgets()) if (auto value = qobject_cast<SingleFileWindow *>(widget)) if (value->document() && value->document()->path() == path) { pane = value; return true; } return false; })());
        auto *session = pane->document()->session(); const auto identity = session->documentId();
        QCOMPARE(session->replace(0, 0, "edited ", session->revision()), EditResult::Applied); const auto edited = session->snapshot().utf8;
        QTimer choose; choose.setInterval(10); connect(&choose, &QTimer::timeout, qApp, [] { for (auto widget : QApplication::topLevelWidgets()) if (widget->objectName() == "unsavedConfirmation") if (auto button = widget->findChild<SharedButton *>("choice2")) button->click(); }); choose.start();
        auto release = std::make_shared<std::atomic_bool>(false);
        const auto unblock = qScopeGuard([release] { release->store(true); });
        auto future = QtConcurrent::run([release] { while (!release->load()) QThread::msleep(5); });
        bool ready = false; app.prepareForUpdate([&](bool value) { ready = value; });
        QTRY_VERIFY(updateTasksBlocked()); QVERIFY(!ready); QVERIFY(!pane->isEnabled()); QCOMPARE(session->documentId(), identity); QCOMPARE(session->snapshot().utf8, edited);
        release->store(true); future.waitForFinished(); QTRY_VERIFY(ready);
        app.cancelUpdateHandoff(); QVERIFY(!updateTasksBlocked()); QVERIFY(pane->isEnabled()); QVERIFY(!session->isReadOnly());
        QCOMPARE(session->documentId(), identity); QCOMPARE(session->snapshot().utf8, edited); QCOMPARE(session->undo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, QByteArray("# owned\n")); QCOMPARE(session->redo(), EditResult::Applied); QCOMPARE(session->snapshot().utf8, edited);
        choose.stop();
    }
    void checkingAndCancelPreserveLiveTextUndoAndSaveBaseline() {
        DocumentSession session("# owned sample\n");const auto original=session.snapshot();
        Network network;Response held;held.hold=true;network.responses={held};
        UpdateController updates(&network,current,"windows","x86_64",false);updates.check();QVERIFY(updates.busy());
        QCOMPARE(session.replace(0,0,QStringLiteral("edited\n"),session.revision()),EditResult::Applied);
        const auto edited=session.snapshot();QVERIFY(session.isDirty());updates.cancel();
        QCOMPARE(session.documentId(),original.documentId);QCOMPARE(session.snapshot().utf8,edited.utf8);
        QCOMPARE(session.undo(),EditResult::Applied);QCOMPARE(session.snapshot().utf8,original.utf8);
        QCOMPARE(session.redo(),EditResult::Applied);QCOMPARE(session.snapshot().utf8,edited.utf8);
        QCOMPARE(session.confirmSaved(session.snapshot()),EditResult::Applied);QVERIFY(!session.isDirty());
        QCOMPARE(updates.state(),"cancelled");
    }
    void synchronousCancelBeforeDispatchDoesNotCreateARequest() {
        Network network;UpdateController updates(&network,current,"windows","x86_64",false);
        connect(&updates,&UpdateController::changed,&updates,[&updates] { if(updates.busy())updates.cancel(); });
        updates.check();QCOMPARE(updates.state(),"cancelled");QCOMPARE(network.requests.size(),0);
    }
    void paginationIsBoundedAndNeverUsesPublicationOrder() {
        QJsonArray full;
        for(int i=0;i<100;++i)full.append(release("0.0.1"));
        Network network;Response page;page.bytes=listing(full);
        network.responses={page,Response{}};
        UpdateController updates(&network,current,"windows","x86_64",false);updates.check();
        QTRY_COMPARE(updates.state(),"up_to_date");QCOMPARE(network.requests.size(),2);QVERIFY(network.requests.last().url().query().endsWith("page=2"));
        network.requests.clear();for(int i=0;i<10;++i)network.responses.append(page);
        updates.check();QTRY_COMPARE(updates.errorCode(),"page_limit");QCOMPARE(network.requests.size(),10);
        full.append(release("0.0.1"));page.bytes=listing(full);network.responses={page};
        updates.check();QTRY_COMPARE(updates.errorCode(),"invalid_response");
    }
    void manifestAndSignatureResponsesHaveTheirOwnLimits() {
        for(const bool signature:{false,true}) {
            Network network;Response list;list.bytes=listing({release()});Response body;body.bytes=signature?manifest():QByteArray(256*1024+1,' ');
            Response sig;sig.bytes=QByteArray(4097,' ');network.responses={list,body,sig};
            UpdateController updates(&network,current,"windows","x86_64",false);updates.check();QTRY_COMPARE(updates.errorCode(),"response_limit");
        }
    }
    void invalidSignatureEnvelopeIsFailureRatherThanUnverifiedAvailability() {
        Network network;Response list;list.bytes=listing({release()});Response body;body.bytes=manifest();Response signature;signature.bytes="broken";
        network.responses={list,body,signature};UpdateController updates(&network,current,"windows","x86_64",false);updates.check();
        QTRY_COMPARE(updates.state(),"error");QCOMPARE(updates.errorCode(),"invalid_signature_envelope");
    }
    void noStartupRequestAndRepeatedClicksShareOneOperation() {
        Network network; Response held;held.hold=true;network.responses={held};
        UpdateController updates(&network,current,"windows","x86_64",false);
        QCOMPARE(network.requests.size(),0);QCOMPARE(updates.state(),"idle");
        updates.check();updates.check();QCOMPARE(network.requests.size(),1);QVERIFY(updates.busy());
        const auto request=network.requests.first();
        QCOMPARE(request.url(),QUrl("https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=1"));
        QVERIFY(!request.hasRawHeader("Authorization")); QVERIFY(!request.hasRawHeader("Cookie"));
        QCOMPARE(request.transferTimeout(),15000);QCOMPARE(request.maximumRedirectsAllowed(),5);
        QCOMPARE(request.sslConfiguration().peerVerifyMode(),QSslSocket::VerifyPeer);
        QCOMPARE(request.sslConfiguration().protocol(),QSsl::TlsV1_2OrLater);
        QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(),int(QNetworkRequest::Manual));
        updates.cancel();QCOMPARE(updates.state(),"cancelled");QVERIFY(!network.exhausted);
    }
    void storeAndUnknownBuildNeverRequestGitHub() {
        Network network;UpdateController store(&network,current,"windows","x86_64",true);store.check();QCOMPARE(store.state(),"store");
        UpdateController unknown(&network,"0.1.0-dev.unknown","windows","x86_64",false);unknown.check();QCOMPARE(unknown.errorCode(),"unknown_build");
        QCOMPARE(network.requests.size(),0);
    }
    void successfulEmptyListAndEtag304AreDistinctFromFailure() {
        Network network;Response first;first.headers={{"ETag","\"fixture\""}};Response cached;cached.status=304;
        network.responses={first,cached};UpdateController updates(&network,current,"windows","x86_64",false);
        updates.check();QTRY_COMPARE(updates.state(),"up_to_date");updates.check();QTRY_COMPARE(updates.state(),"up_to_date");
        QCOMPARE(network.requests[1].rawHeader("If-None-Match"),QByteArray("\"fixture\""));
        Response missing;missing.status=304;network.responses={missing};
        UpdateController fresh(&network,current,"windows","x86_64",false);fresh.check();QTRY_COMPARE(fresh.errorCode(),"invalid_cache");
    }
    void failuresNeverReportUpToDate_data() {
        QTest::addColumn<int>("status");QTest::addColumn<QByteArray>("body");QTest::addColumn<int>("error");QTest::addColumn<QString>("expected");
        QTest::newRow("offline")<<0<<QByteArray()<<int(QNetworkReply::ConnectionRefusedError)<<QString("network");
        QTest::newRow("timeout")<<0<<QByteArray()<<int(QNetworkReply::TimeoutError)<<QString("timeout");
        QTest::newRow("tls")<<0<<QByteArray()<<int(QNetworkReply::SslHandshakeFailedError)<<QString("tls");
        QTest::newRow("forbidden")<<403<<QByteArray("{\"message\":\"Forbidden\"}")<<int(QNetworkReply::ContentAccessDenied)<<QString("forbidden");
        QTest::newRow("rate-429")<<429<<QByteArray()<<int(QNetworkReply::ContentAccessDenied)<<QString("rate_limited");
        QTest::newRow("rate-403")<<403<<QByteArray("{\"message\":\"API rate limit exceeded\"}")<<int(QNetworkReply::ContentAccessDenied)<<QString("rate_limited");
        QTest::newRow("broken-json")<<200<<QByteArray("{broken")<<int(QNetworkReply::NoError)<<QString("invalid_response");
        QTest::newRow("overflow")<<200<<QByteArray(8*1024*1024+1,' ')<<int(QNetworkReply::NoError)<<QString("response_limit");
    }
    void failuresNeverReportUpToDate() {
        QFETCH(int,status);QFETCH(QByteArray,body);QFETCH(int,error);QFETCH(QString,expected);
        Network network;Response response;response.status=status;response.bytes=body;response.error=QNetworkReply::NetworkError(error);network.responses={response};
        UpdateController updates(&network,current,"windows","x86_64",false);updates.check();QTRY_COMPARE(updates.state(),"error");QCOMPARE(updates.errorCode(),expected);
    }
    void redirectAndTlsSignalsCannotReuseCachedSuccess() {
        for (const bool tls:{false,true}) {
            Network network;Response response;response.tls=tls;response.redirect=QUrl("http://github.com/unsafe");network.responses={response};
            UpdateController updates(&network,current,"windows","x86_64",false);updates.check();QTRY_COMPARE(updates.state(),"error");QCOMPARE(updates.errorCode(),tls?"tls":"invalid_redirect");
        }
        QVERIFY(UpdateController::allowedUrl(QUrl("https://release-assets.githubusercontent.com/path?token=temporary")));
        for (const auto &url:{"http://github.com/x","https://github.com.attacker.invalid/x","https://user@github.com/x","https://github.com:444/x","https://github.com/x#fragment"}) QVERIFY(!UpdateController::allowedUrl(QUrl(url)));
    }
    void newestSemverManifestAndRawSignatureUseOneCandidate() {
        Network network;Response list;list.bytes=listing({release("0.1.1"),release("0.2.0")});
        Response body;body.bytes=manifest();body.finalOnly=true;Response signature;signature.bytes=R"({"schema_version":1,"algorithm":"ed25519","key_id":"unconfigured","signature":""})";
        network.responses={list,body,signature};UpdateController updates(&network,current,"windows","x86_64",false);
        updates.check();QTRY_COMPARE(updates.state(),"available");QCOMPARE(updates.candidateVersion(),"0.2.0");QVERIFY(!updates.trusted());
        QCOMPARE(network.requests.size(),3);QVERIFY(network.requests[1].url().path().contains("v0.2.0/release.json"));
        QVERIFY(!network.exhausted);
    }
    void invalidAssetAndManifestIdentityDoNotFallBackToOlderRelease() {
        for (const bool wrongTag:{false,true}) {
            Network network;Response list;auto selected=release();
            if (!wrongTag) {auto assets=selected["assets"].toArray();auto item=assets[2].toObject();item["size"]=13;assets[2]=item;selected["assets"]=assets;}
            list.bytes=listing({selected,release("0.1.1")});Response body;body.bytes=manifest(wrongTag?"0.3.0":"0.2.0");network.responses={list,body};
            UpdateController updates(&network,current,"windows","x86_64",false);updates.check();QTRY_COMPARE(updates.state(),"error");QCOMPARE(updates.errorCode(),wrongTag?"release_identity":"release_asset");QCOMPARE(network.requests.size(),2);
        }
    }
    void cancelledReplyCannotOverwriteNextCheck() {
        Network network;Response hold;hold.hold=true;network.responses={hold,Response{}};
        UpdateController updates(&network,current,"windows","x86_64",false);updates.check();updates.cancel();updates.check();QTRY_COMPARE(updates.state(),"up_to_date");
        QCOMPARE(network.requests.size(),2);
    }
    void mirrorAndProxyValidationStayFrozenDuringOperation() {
        QVERIFY(UpdateNetwork::validProxy("socks5://127.0.0.1:1080"));QVERIFY(!UpdateNetwork::validProxy("http://user:password@localhost:8080"));
        for (const auto &url:{"http://mirror.invalid","https://mirror.invalid/a/../b","https://user@mirror.invalid","https://mirror.invalid/%2e%2e"}) QVERIFY(!UpdateNetwork::validMirror(url));
        Network network;Response list;list.bytes=listing({release()});Response body;body.bytes=manifest();Response sig;sig.bytes=R"({"schema_version":1,"algorithm":"ed25519","key_id":"unknown","signature":""})";network.responses={list,body,sig};
        UpdateController updates(&network,current,"windows","x86_64",false);QVERIFY(updates.configureNetwork("none",{},"https://mirror.invalid/prefix"));updates.check();
        QVERIFY(updates.configureNetwork("custom","http://localhost:8080","https://next.invalid"));QTRY_COMPARE(updates.state(),"available");
        for (const auto &request:network.requests) QCOMPARE(request.url().host(),"mirror.invalid");
        const auto proxies=network.proxyFactory()->queryProxy(QNetworkProxyQuery(QUrl("https://github.com")));QCOMPARE(proxies.first().type(),QNetworkProxy::NoProxy);
    }
    void aboutReopenPreservesControllerAndScreenshots() {
        QTemporaryDir dir;Appearance appearance(nullptr,dir.filePath("settings.json"));Network network;Response hold;hold.hold=true;network.responses={hold};
        UpdateController updates(&network,current,"windows","x86_64",false);
        auto *window=new SettingsWindow(&appearance,nullptr,&updates);window->show();window->selectPage(5);
        auto *check=window->findChild<SharedButton*>("settingsCheckUpdate");QVERIFY(check);
        auto *pages=window->findChild<QStackedWidget*>("settingsPages");auto *scroll=qobject_cast<QScrollArea*>(pages->currentWidget());QVERIFY(scroll);scroll->ensureWidgetVisible(check);QTest::qWait(20);
        QTest::mouseClick(check,Qt::LeftButton);QCOMPARE(network.requests.size(),1);
        QPointer<SettingsWindow> old=window;window->close();QTRY_VERIFY(old.isNull());QVERIFY(updates.busy());
        window=new SettingsWindow(&appearance,nullptr,&updates);window->show();window->selectPage(5);
        QVERIFY(!window->findChild<SharedButton*>("settingsCheckUpdate")->isEnabled());QCOMPARE(network.requests.size(),1);
        QTest::mouseClick(window->findChild<SharedButton*>("settingsCancelUpdate"),Qt::LeftButton);QCOMPARE(updates.state(),"cancelled");
        const auto root=qEnvironmentVariable("XDNOTE_TEST_EVIDENCE");
        if (!root.isEmpty()) {
            QVERIFY(QDir().mkpath(root+"/about"));
            for (const auto &theme:{"light","dark"}) {
                if(updates.state()=="error") { Response held;held.hold=true;network.responses={held};updates.check();updates.cancel(); }
                QVERIFY(appearance.setPreferences(QString::fromLatin1(theme), appearance.accent()));
                window->resize(1080,820);QTest::qWait(50);
                auto *version=window->findChild<QLabel*>("settingsBuildVersion");QVERIFY(version);QVERIFY(!version->wordWrap());QVERIFY(version->text().startsWith(BuildInfo::version()));
                pages=window->findChild<QStackedWidget*>("settingsPages");scroll=qobject_cast<QScrollArea*>(pages->currentWidget());scroll->ensureWidgetVisible(window->findChild<QLabel*>("settingsUpdateMessage"));QTest::qWait(20);
                QVERIFY(window->grab().save(root+"/about/"+theme+".png"));
                Response failed;failed.status=403;network.responses={failed};updates.check();QTRY_COMPARE(updates.state(),"error");
                QTest::qWait(20);QVERIFY(window->grab().save(root+"/about/"+theme+"-error.png"));
            }
        }
        window->close();
    }
};
int main(int argc,char **argv) { initializeGuiRuntime();QApplication app(argc,argv);configureGuiApplication();app.setQuitOnLastWindowClosed(false);UpdateTests tests;return QTest::qExec(&tests,argc,argv); }
#include "update_tests.moc"
