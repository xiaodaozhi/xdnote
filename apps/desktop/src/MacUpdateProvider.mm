// apps/desktop/MacUpdateProvider.mm

#include "MacUpdateProvider.h"
using namespace xdnote;
#include "UpdateController.h"
#include "UpdateNetwork.h"
#import <AppKit/AppKit.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTimer>
#ifdef XDNOTE_UPDATE_TESTS
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#endif
#import <Sparkle/Sparkle.h>
#if SPU_XDNOTE_DOWNLOAD_PROXY_API != 1
#error The pinned xdnote Sparkle download proxy API is required.
#endif
#include <cstdint>
#include <memory>
#include <utility>

// Sparkle owns extraction, authorization, replacement, and relaunch. Only the
// application's completed join barrier is allowed to release installReply.
@interface XdnoteUpdateDriver : NSObject <SPUUserDriver, SPUUpdaterDelegate> {
  @public
    QPointer<UpdateController> owner;
    QJsonObject candidate;
    MacUpdateTransport transport;
    std::function<void(QString)> prepared;
    std::function<void()> deferredQuit;
    SPUUpdater *updater;
    void (^installReply)(SPUUserUpdateChoice);
    void (^cancelDownload)(void);
    uint64_t received;
    uint64_t total;
    BOOL preparationReported;
    BOOL cancelled;
    BOOL committed;
    BOOL cycleFinished;
    BOOL installerRequired;
    BOOL readyObservationPending;
    BOOL installerSeenAtReady;
    BOOL installerGone;
    BOOL quitObservationPending;
    BOOL installerObservationError;
    QString installerPath;
    QSet<qint64> readyInstallerPids;
}
- (BOOL)matches:(SUAppcastItem *)item;
- (void)failure;
- (void)finishDeferredQuit;
@end

namespace {
XdnoteUpdateDriver *driver = nil;
QPointer<QObject> commitContext;
std::function<void()> commitCallback;
bool updateSessionBlocked() {
    return driver && (driver->updater.sessionInProgress ||
                      (driver->installerRequired && !driver->installerGone));
}
NSString *native(const QString &value) {
    return [NSString stringWithUTF8String:value.toUtf8().constData()];
}
QString text(NSString *value) {
    return value ? QString::fromUtf8(value.UTF8String) : QString{};
}
using InstallerObservation = std::function<void(bool, QSet<qint64>)>;
bool installerPids(const QByteArray &output, const QString &path, QSet<qint64> &pids) {
    if (output.trimmed().isEmpty())
        return false;
    for (const auto &line : output.split('\n')) {
        const auto value = line.trimmed();
        if (value.isEmpty())
            continue;
        const auto separator = value.indexOf(' ');
        if (separator < 1)
            return false;
        bool number = false;
        const auto pid = value.first(separator).toLongLong(&number);
        if (!number || pid < 1)
            return false;
        const auto executable = QString::fromUtf8(value.mid(separator + 1).trimmed());
        if (QFileInfo(executable).fileName() == "Autoupdate" &&
            QFileInfo(executable).canonicalFilePath() == path)
            pids.insert(pid);
    }
    return true;
}
#ifdef XDNOTE_UPDATE_TESTS
std::function<void(InstallerObservation)> installerObservationOverride;
#endif
void observeInstaller(const QString &path, InstallerObservation completion) {
#ifdef XDNOTE_UPDATE_TESTS
    if (installerObservationOverride) {
        installerObservationOverride(std::move(completion));
        return;
    }
#endif
    // /bin/ps is the public OS inspection tool. Read only PID and executable
    // path, never arguments/environment, and retain only exact bundled-helper
    // matches. Neither process listings nor unrelated paths enter product logs.
    if (path.isEmpty() || path.contains('\n') || path.contains('\r') || path.contains('\t')) {
        completion(false, {});
        return;
    }
    struct Query {
        QByteArray output;
        bool completed = false, invalid = false;
    };
    const auto query = std::make_shared<Query>();
    auto *process = new QProcess(qApp);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "en_US.UTF-8");
    process->setProcessEnvironment(environment);
    const auto finish = [process, query, completion = std::move(completion)](bool valid,
                                                                             QSet<qint64> pids) {
        if (query->completed)
            return;
        query->completed = true;
        completion(valid, std::move(pids));
        process->deleteLater();
    };
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [=] {
        query->output += process->readAllStandardOutput();
        if (query->output.size() > 4 * 1024 * 1024) {
            query->invalid = true;
            process->kill(); // Only this owned inspection process, never the installer.
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [=](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, {});
    });
    QObject::connect(
        process, &QProcess::finished, process, [=](int status, QProcess::ExitStatus exit) {
            query->output += process->readAllStandardOutput();
            if (query->invalid || status || exit != QProcess::NormalExit ||
                query->output.trimmed().isEmpty() || query->output.size() > 4 * 1024 * 1024) {
                finish(false, {});
                return;
            }
            QSet<qint64> pids;
            const bool valid = installerPids(query->output, path, pids);
            finish(valid, std::move(pids));
        });
    QTimer::singleShot(5000, process, [=] {
        if (!query->completed) {
            query->invalid = true;
            process->kill();
        }
    });
    process->start(QStringLiteral("/bin/ps"), {"-Aww", "-o", "pid=", "-o", "comm="});
}
} // namespace

@implementation XdnoteUpdateDriver
- (BOOL)matches:(SUAppcastItem *)item {
    const bool accepted =
        item.signingValidationStatus == SPUAppcastSigningValidationStatusSucceeded &&
        !item.informationOnlyUpdate &&
        text(item.versionString) == QString::number(candidate.value("build").toInt()) &&
        text(item.displayVersionString) == candidate.value("version").toString() &&
        text(item.fileURL.absoluteString) == candidate.value("asset_url").toString() &&
        item.contentLength == quint64(candidate.value("bytes").toDouble());
    return accepted;
}
- (void)failure {
    if (cancelled)
        return;
    if (prepared) {
        auto callback = std::exchange(prepared, {});
        callback(QStringLiteral("native_update"));
    }
    if (owner && !cancelled)
        owner->reportNativeProgress(QStringLiteral("error"), -1);
}
- (NSString *)feedURLStringForUpdater:(SPUUpdater *)sender {
    (void)sender;
    const auto original = QUrl(candidate.value("appcast_url").toString());
    return native(UpdateNetwork::mapped(original, transport.mirror).toString());
}
- (BOOL)updaterShouldPromptForPermissionToCheckForUpdates:(SPUUpdater *)sender {
    (void)sender;
    return NO;
}
- (NSArray<NSString *> *)allowedSystemProfileKeysForUpdater:(SPUUpdater *)sender {
    (void)sender;
    return @[];
}
- (NSSet<NSString *> *)allowedChannelsForUpdater:(SPUUpdater *)sender {
    (void)sender;
    NSString *channel = native(candidate.value("channel").toString());
    return [NSSet setWithObject:channel];
}
- (BOOL)updater:(SPUUpdater *)sender
    shouldProceedWithUpdate:(SUAppcastItem *)item
                updateCheck:(SPUUpdateCheck)check
                      error:(NSError *__autoreleasing *)error {
    (void)sender;
    (void)check;
    if (!cancelled && [self matches:item])
        return YES;
    if (error)
        *error = [NSError errorWithDomain:@"org.xdnote.update"
                                     code:1
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         @"The selected update does not match its verified release."
                                 }];
    return NO;
}
- (BOOL)updater:(SPUUpdater *)sender shouldDownloadReleaseNotesForUpdate:(SUAppcastItem *)item {
    (void)sender;
    (void)item;
    return NO;
}
- (void)updater:(SPUUpdater *)sender
    willDownloadUpdate:(SUAppcastItem *)item
           withRequest:(NSMutableURLRequest *)request {
    (void)sender;
    const bool accepted = [self matches:item];
    const auto original = text(request.URL.absoluteString);
    const auto mirror = transport.mirror;
    const QUrl originalUrl(original, QUrl::StrictMode);
    const auto mapped = UpdateNetwork::mapped(originalUrl, mirror);
    const bool valid = accepted && original == candidate.value("asset_url").toString() &&
                       UpdateNetwork::validMirror(mirror) &&
                       (mirror.isEmpty() || UpdateNetwork::isMirrorUrl(mapped, mirror));
    request.URL = valid ? [NSURL URLWithString:native(mapped.toString())]
                        : [NSURL URLWithString:@"xdnote-owned-rejected://archive"];
}
- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest *)request
                              reply:(void (^)(SUUpdatePermissionResponse *))reply {
    (void)request;
    reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:NO
                                                          sendSystemProfile:NO]);
}
- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {
    if (cancelled) {
        cancellation();
        return;
    }
    cancelDownload = [cancellation copy];
    if (owner)
        owner->reportNativeProgress(QStringLiteral("checking"), -1);
}
- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem *)item
                                 state:(SPUUserUpdateState *)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply {
    cancelDownload = nil;
    if (cancelled || !owner || ![self matches:item]) {
        reply(SPUUserUpdateChoiceSkip);
        [self failure];
        return;
    }
    if (state.stage == SPUUserUpdateStageInstalling) {
        [self showReadyToInstallAndRelaunch:reply];
    } else {
        reply(SPUUserUpdateChoiceInstall);
    }
}
- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData *)data {
    (void)data;
}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError *)error {
    (void)error;
}
- (void)showUpdateNotFoundWithError:(NSError *)error
                    acknowledgement:(void (^)(void))acknowledgement {
    (void)error;
    cancelDownload = nil;
    [self failure];
    acknowledgement();
}
- (void)showUpdaterError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement {
    (void)error;
    cancelDownload = nil;
    [self failure];
    acknowledgement();
}
- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
    if (cancelled) {
        cancellation();
        return;
    }
    cancelDownload = [cancellation copy];
    received = 0;
    total = 0;
    if (owner)
        owner->reportNativeProgress(QStringLiteral("downloading"), -1);
}
- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)length {
    total = length;
}
- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
    if (cancelled)
        return;
    if (UINT64_MAX - received < length) {
        [self failure];
        return;
    }
    received += length;
    if (owner)
        owner->reportNativeProgress(QStringLiteral("downloading"),
                                    total ? qMin(qreal(1), qreal(received) / qreal(total)) : -1);
}
- (void)showDownloadDidStartExtractingUpdate {
    installerRequired = YES;
    cancelDownload = nil;
    if (!cancelled && owner)
        owner->reportNativeProgress(QStringLiteral("verifying"), -1);
}
- (void)showExtractionReceivedProgress:(double)progress {
    if (!cancelled && owner)
        owner->reportNativeProgress(QStringLiteral("verifying"), progress);
}
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    // Cancellation can arrive during extraction, when Sparkle offers no download
    // cancellation block. Cancel the pending installer when it next becomes ready.
    if (installReply || readyObservationPending) {
        reply(SPUUserUpdateChoiceSkip);
        [self failure];
        return;
    }
    installerRequired = YES;
    readyObservationPending = YES;
    // Hold even the cancellation reply until we have seen the real helper.
    // Otherwise a fast cancellation can make an absent process look like a
    // successful observation without ever binding it to this ready installer.
    observeInstaller(installerPath, [self, reply](bool valid, QSet<qint64> pids) {
        self->readyObservationPending = NO;
        if (!valid || pids.isEmpty()) {
            self->installerObservationError = YES;
            reply(SPUUserUpdateChoiceSkip);
            [self failure];
            return;
        }
        self->installerSeenAtReady = YES;
        self->readyInstallerPids = std::move(pids);
        if (self->cancelled || !self->owner) {
            reply(SPUUserUpdateChoiceSkip);
            return;
        }
        self->installReply = [reply copy];
        if (!self->preparationReported && self->prepared) {
            self->preparationReported = YES;
            auto callback = std::exchange(self->prepared, {});
            callback({});
        }
    });
}
- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)terminated
                          retryTerminatingApplication:(void (^)(void))retry {
    if (owner)
        owner->reportNativeProgress(QStringLiteral("installing"), -1);
    (void)retry;
    // Sparkle has already started its installer before this callback. The Rust
    // join barrier released commitMacUpdate(), so the normal Qt quit is now safe.
    // Do not repeatedly invoke retry while Cocoa is processing its quit request.
    if (!terminated && committed && owner)
        emit owner->quitForUpdate();
}
- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched
                         acknowledgement:(void (^)(void))acknowledgement {
    (void)relaunched;
    acknowledgement();
}
- (void)dismissUpdateInstallation {
    cancelDownload = nil;
    installReply = nil;
}
- (BOOL)updaterShouldRelaunchApplication:(SPUUpdater *)sender {
    (void)sender;
    return committed && !cancelled;
}
- (void)updater:(SPUUpdater *)sender
    didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)check
                                 error:(NSError *)error {
    (void)check;
    (void)error;
    // SPUUpdater retains its user driver. Release our reverse reference after
    // Sparkle returns from the completed cycle, so retries do not leak a pair.
    QTimer::singleShot(0, qApp, [self, sender] {
        if (self->updater == sender)
            self->updater = nil;
        self->cycleFinished = YES;
        [self finishDeferredQuit];
    });
}
- (void)finishDeferredQuit {
    if (!cycleFinished || quitObservationPending ||
        (!deferredQuit && !(installerRequired && !installerGone && !committed)))
        return;
    if (!installerRequired) {
        auto quit = std::exchange(deferredQuit, {});
        if (quit)
            quit();
        return;
    }
    quitObservationPending = YES;
    observeInstaller(installerPath, [self](bool valid, QSet<qint64> pids) {
        self->quitObservationPending = NO;
        if (valid && pids.isEmpty()) {
            self->installerGone = YES;
            auto quit = std::exchange(self->deferredQuit, {});
            if (quit)
                quit();
            return;
        }
        if (!valid) {
            self->installerObservationError = YES;
            [self failure];
        }
        // The cycle callback only proves the cancel message was sent. The
        // independent helper must actually exit before the app can terminate.
        // A failed observation never releases this barrier or kills a helper.
        QTimer::singleShot(100, qApp, [self] { [self finishDeferredQuit]; });
    });
}
@end

QString macUpdateInstallRoot() {
    NSBundle *bundle = NSBundle.mainBundle;
    NSURL *url = bundle.bundleURL;
    if (![bundle.bundleIdentifier isEqualToString:@"io.github.xiaodaozhi.xdnote"] ||
        ![[bundle objectForInfoDictionaryKey:@"XdnoteUpdateInstallable"] boolValue] ||
        ![[bundle objectForInfoDictionaryKey:@"XdnoteUpdateInstallId"]
            isEqualToString:@"io.github.xiaodaozhi.xdnote.release.v1"] ||
        ![[bundle objectForInfoDictionaryKey:@"SUPublicEDKey"] length])
        return {};
    const auto path = text(url.path);
    const QFileInfo installation(path);
    const auto parent = installation.dir().canonicalPath();
    if (installation.isSymLink() ||
        (parent != QStringLiteral("/Applications") &&
         parent !=
             QFileInfo(QDir::homePath() + QStringLiteral("/Applications")).canonicalFilePath()))
        return {};
    if (!path.endsWith(".app") || path.startsWith("/Volumes/") ||
        path.contains("/AppTranslocation/"))
        return {};
    NSNumber *readOnly = nil;
    if (![url getResourceValue:&readOnly forKey:NSURLVolumeIsReadOnlyKey error:nil] ||
        readOnly.boolValue)
        return {};
    // Sparkle handles normal authorization for an installed, non-writable app.
    return path;
}
void startMacUpdate(UpdateController *owner, const QJsonObject &candidate,
                    std::function<void(QString)> prepared, const MacUpdateTransport &transport) {
    if (updateSessionBlocked()) {
        prepared("update_busy");
        return;
    }
    if (!UpdateNetwork::macosAutomaticTransport(transport.proxyMode, transport.proxyAddress) ||
        !UpdateNetwork::validMirror(transport.mirror)) {
        prepared("native_update");
        return;
    }
    driver = [[XdnoteUpdateDriver alloc] init];
    driver->owner = owner;
    driver->candidate = candidate;
    driver->transport = transport;
    driver->prepared = std::move(prepared);
    const auto framework = [NSBundle bundleForClass:SPUUpdater.class];
    const auto helper = [framework.executableURL.URLByDeletingLastPathComponent
        URLByAppendingPathComponent:@"Autoupdate"];
    driver->installerPath = QFileInfo(text(helper.path)).canonicalFilePath();
    driver->updater = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle
                                           applicationBundle:NSBundle.mainBundle
                                                  userDriver:driver
                                                    delegate:driver];
    driver->updater.automaticallyChecksForUpdates = NO;
    driver->updater.automaticallyDownloadsUpdates = NO;
    NSError *error = nil;
    NSURL *proxyURL = transport.proxyMode == "custom"
                         ? [NSURL URLWithString:native(transport.proxyAddress)]
                         : nil;
    const auto proxyMode = transport.proxyMode == "none"     ? SPUXdnoteDownloadProxyDirect
                           : transport.proxyMode == "custom" ? SPUXdnoteDownloadProxyCustom
                                                              : SPUXdnoteDownloadProxySystem;
    if ((transport.proxyMode == "custom" && proxyURL == nil) ||
        ![driver->updater xdnote_setDownloadProxyMode:proxyMode proxyURL:proxyURL error:&error]) {
        [driver failure];
        driver->updater = nil;
        return;
    }
    if (![driver->updater startUpdater:&error]) {
        [driver failure];
        driver->updater = nil;
        return;
    }
    [driver->updater checkForUpdates];
}
bool macUpdateInstallationCommitted() {
    return driver && driver->committed;
}
void setMacUpdateCommitCallback(QObject *context, std::function<void()> callback) {
    commitContext = context;
    commitCallback = std::move(callback);
}
bool commitMacUpdate() {
    if (!driver || driver->cancelled || driver->committed || !driver->installReply)
        return false;
    driver->committed = YES;
    auto reply = driver->installReply;
    driver->installReply = nil;
    if (commitContext && commitCallback)
        commitCallback();
    reply(SPUUserUpdateChoiceInstall);
    return true;
}
void cancelMacUpdate() {
    if (!driver)
        return;
    if (driver->committed)
        return;
    driver->cancelled = YES;
    driver->prepared = {};
    if (driver->installReply) {
        auto reply = driver->installReply;
        driver->installReply = nil;
        // Dismiss would leave an installation scheduled for the next quit.
        reply(SPUUserUpdateChoiceSkip);
    }
    if (driver->cancelDownload) {
        auto cancel = driver->cancelDownload;
        driver->cancelDownload = nil;
        cancel();
    }
}
bool deferMacUpdateQuit(std::function<void()> quit) {
    if (!driver || driver->committed ||
        (!driver->updater.sessionInProgress &&
         !(driver->installerRequired && !driver->installerGone)))
        return false;
    if (!driver->deferredQuit)
        driver->deferredQuit = std::move(quit);
    cancelMacUpdate();
    [driver finishDeferredQuit];
    return true;
}
QJsonObject macUpdateExitStatus() {
    QJsonArray pids;
    if (driver)
        for (const auto pid : driver->readyInstallerPids)
            pids.append(pid);
    return {{"installer_seen_at_ready", driver && driver->installerSeenAtReady},
            {"committed", macUpdateInstallationCommitted()},
            {"installer_pids_at_ready", pids},
            {"installer_gone_before_exit", driver && driver->installerGone},
            {"installer_observation_error", driver && driver->installerObservationError},
            {"native_cycle_finished", driver && driver->cycleFinished}};
}
#ifdef XDNOTE_UPDATE_TESTS
bool macUpdateLifecycleProbe(UpdateController *owner) {
    // Exercise the real Sparkle user-driver callbacks, without starting a
    // network session, authorizing a helper, or modifying an installed app.
    auto previous = driver;
    const auto previousCommitContext = commitContext;
    auto previousCommitCallback = std::exchange(commitCallback, {});
    commitContext = nullptr;
    QTemporaryDir parserDirectory;
    const auto helper = parserDirectory.path() + QStringLiteral("/用户目录/更新.app/Autoupdate");
    QDir().mkpath(QFileInfo(helper).absolutePath());
    QFile parserHelper(helper);
    const bool helperCreated = parserHelper.open(QIODevice::WriteOnly);
    parserHelper.close();
    QSet<qint64> parsedPids;
    const bool unicodeParser =
        helperCreated &&
        installerPids("  123 " + helper.toUtf8() + "\n 124 /unrelated/command\\nescaped\n",
                      QFileInfo(helper).canonicalFilePath(), parsedPids) &&
        parsedPids == QSet<qint64>{123};
    QSet<qint64> malformedPids;
    const bool malformedRejected = !installerPids("malformed\n", helper, malformedPids);
    auto previousObservation = installerObservationOverride;
    installerObservationOverride = [](InstallerObservation completion) { completion(true, {42}); };
    driver = [[XdnoteUpdateDriver alloc] init];
    driver->owner = owner;
    __block int cancelledReplies = 0;
    __block bool cancelledWithSkip = false;
    bool preparedAfterCancellation = false;
    driver->prepared = [&](QString) { preparedAfterCancellation = true; };
    [driver showDownloadDidStartExtractingUpdate];
    cancelMacUpdate();
    [driver showReadyToInstallAndRelaunch:^(SPUUserUpdateChoice choice) {
      ++cancelledReplies;
      cancelledWithSkip = choice == SPUUserUpdateChoiceSkip;
    }];
    const bool cancellationPassed = cancelledReplies == 1 && cancelledWithSkip &&
                                    !preparedAfterCancellation && !commitMacUpdate();
    driver = [[XdnoteUpdateDriver alloc] init];
    driver->owner = owner;
    int preparedCount = 0;
    __block int installReplies = 0;
    __block bool installed = false;
    __block bool committedBeforeReply = false;
    QObject commitLifetime;
    int commitReleases = 0;
    const auto releaseCount = &commitReleases;
    setMacUpdateCommitCallback(&commitLifetime, [&] {
        if (macUpdateInstallationCommitted())
            ++commitReleases;
    });
    driver->prepared = [&](QString error) {
        if (error.isEmpty())
            ++preparedCount;
    };
    [driver showReadyToInstallAndRelaunch:^(SPUUserUpdateChoice choice) {
      ++installReplies;
      installed = choice == SPUUserUpdateChoiceInstall;
      committedBeforeReply = macUpdateInstallationCommitted() && *releaseCount == 1;
    }];
    const bool held = preparedCount == 1 && installReplies == 0 &&
                      !macUpdateInstallationCommitted() && commitReleases == 0;
    const bool committedOnce = commitMacUpdate() && !commitMacUpdate();
    const bool installationPassed = held && committedOnce && installed && installReplies == 1 &&
                                    committedBeforeReply && commitReleases == 1;
    driver = [[XdnoteUpdateDriver alloc] init];
    bool immediateQuit = false;
    const bool inactiveQuit = !deferMacUpdateQuit([&] { immediateQuit = true; });
    QEventLoop finished;
    int completedQuits = 0;
    int helperObservations = 0;
    driver->installerRequired = YES;
    driver->installerSeenAtReady = YES;
    driver->readyInstallerPids = {42};
    installerObservationOverride = [&](InstallerObservation completion) {
        ++helperObservations;
        completion(true, helperObservations == 1 ? QSet<qint64>{42} : QSet<qint64>{});
    };
    const bool activeQuitDeferred = deferMacUpdateQuit([&] {
        ++completedQuits;
        finished.quit();
    });
    // Supply the SDK's required non-null sender without starting an updater.
    auto cycleSender = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle
                                            applicationBundle:NSBundle.mainBundle
                                                   userDriver:driver
                                                     delegate:driver];
    driver->updater = cycleSender;
    [driver updater:cycleSender didFinishUpdateCycleForUpdateCheck:SPUUpdateCheckUpdates error:nil];
    const bool heldUntilCycleReturns = completedQuits == 0;
    QTimer::singleShot(1000, &finished, &QEventLoop::quit);
    finished.exec();
    const bool normalCallbackCleared = !driver->deferredQuit;
    const bool normalQuitPassed = inactiveQuit && !immediateQuit && heldUntilCycleReturns &&
                                  activeQuitDeferred && completedQuits == 1 &&
                                  helperObservations == 2 && driver->installerGone &&
                                  normalCallbackCleared;
    bool rejectedReadyPassed = true;
    QJsonArray rejectedObservations;
    for (const bool validObservation : {false, true}) {
        driver = [[XdnoteUpdateDriver alloc] init];
        driver->owner = owner;
        bool rejectedPreparation = false;
        driver->prepared = [&](QString error) { rejectedPreparation = error == "native_update"; };
        installerObservationOverride = [=](InstallerObservation completion) {
            completion(validObservation, {});
        };
        __block bool rejectedWithSkip = false;
        [driver showReadyToInstallAndRelaunch:^(SPUUserUpdateChoice choice) {
          rejectedWithSkip = choice == SPUUserUpdateChoiceSkip;
        }];
        auto rejectedDriver = driver;
        QString retryError;
        startMacUpdate(owner, {}, [&](QString error) { retryError = error; });
        const bool retryHeld = retryError == "update_busy" && driver == rejectedDriver;
        QEventLoop settled;
        int observations = 0;
        bool aliveRetryHeld = false;
        installerObservationOverride = [&](InstallerObservation completion) {
            ++observations;
            if (observations == 1) {
                QString error;
                startMacUpdate(owner, {}, [&](QString value) { error = value; });
                aliveRetryHeld = error == "update_busy" && driver == rejectedDriver;
                completion(true, {42});
            } else {
                completion(true, {});
                QTimer::singleShot(0, &settled, &QEventLoop::quit);
            }
        };
        auto rejectedSender = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle
                                                   applicationBundle:NSBundle.mainBundle
                                                          userDriver:driver
                                                            delegate:driver];
        driver->updater = rejectedSender;
        [driver updater:rejectedSender
            didFinishUpdateCycleForUpdateCheck:SPUUpdateCheckUpdates
                                         error:nil];
        QTimer::singleShot(1000, &settled, &QEventLoop::quit);
        settled.exec();
        rejectedReadyPassed &= rejectedWithSkip && rejectedPreparation && retryHeld &&
                               aliveRetryHeld && observations == 2 && driver->installerGone &&
                               !updateSessionBlocked();
        rejectedObservations.append(QJsonObject{{"valid_observation", validObservation},
                                                {"reply_skip", rejectedWithSkip},
                                                {"preparation_rejected", rejectedPreparation},
                                                {"retry_held", retryHeld},
                                                {"alive_retry_held", aliveRetryHeld},
                                                {"observations", observations},
                                                {"helper_gone", bool(driver->installerGone)},
                                                {"retry_gate_released", !updateSessionBlocked()}});
    }
    const QJsonObject diagnostics{
        {"unicode_parser", unicodeParser},
        {"malformed_rejected", malformedRejected},
        {"cancellation", cancellationPassed},
        {"installation", installationPassed},
        {"guard_release_after_commit_before_reply", bool(committedBeforeReply)},
        {"commit_release_calls", commitReleases},
        {"normal_quit", normalQuitPassed},
        {"normal_callback_cleared", normalCallbackCleared},
        {"normal_quit_calls", completedQuits},
        {"normal_helper_observations", helperObservations},
        {"rejected_ready", rejectedReadyPassed},
        {"rejected_observations", rejectedObservations}};
    qInfo().noquote() << "Mac updater lifecycle:"
                      << QJsonDocument(diagnostics).toJson(QJsonDocument::Compact);
    installerObservationOverride = std::move(previousObservation);
    driver = previous;
    setMacUpdateCommitCallback(previousCommitContext.data(), std::move(previousCommitCallback));
    return unicodeParser && malformedRejected && cancellationPassed && installationPassed &&
           normalQuitPassed && rejectedReadyPassed;
}
#endif
