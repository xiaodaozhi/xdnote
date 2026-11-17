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
