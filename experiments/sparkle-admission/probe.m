// Isolated native configuration / loopback TLS probe, never a product updater.
#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>
#import <Network/Network.h>
#import <Network/NSURLSession+Network.h>
#import <CFNetwork/CFNetwork.h>
#import <Security/Security.h>

static void report(NSDictionary *value, int status) {
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    fwrite(bytes.bytes, 1, bytes.length, stdout); fputc('\n', stdout); fflush(stdout);
    exit(status);
}
static NSMutableArray<NSString *> *checks;
@interface ProbeTrustObserver : NSObject <NSURLSessionDelegate>
@property (atomic) BOOL sawUntrustedServerTrust;
@end
@implementation ProbeTrustObserver
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
    (void)session;
    if ([challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] &&
        challenge.protectionSpace.serverTrust != nil && !SecTrustEvaluateWithError(challenge.protectionSpace.serverTrust, NULL)) {
        self.sawUntrustedServerTrust = YES;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}
@end
@interface ProbeUpdaterObserver : NSObject <SPUUpdaterDelegate>
@property (nonatomic, copy) NSURL *feed;
@property (nonatomic) BOOL archiveStarted;
@end
@implementation ProbeUpdaterObserver
- (NSString *)feedURLStringForUpdater:(SPUUpdater *)__unused updater {
    return self.feed.absoluteString;
}
- (BOOL)updater:(SPUUpdater *)__unused updater shouldProceedWithUpdate:(SUAppcastItem *)item updateCheck:(SPUUpdateCheck)__unused type error:(NSError **)__unused error {
    NSURL *archive = item.fileURL;
    return archive.port.intValue > 0 && [archive.path isEqualToString:@"/archive.zip"] &&
        [archive.scheme isEqualToString:@"https"] &&
        ([archive.host isEqualToString:@"127.0.0.1"] ||
         [archive.host isEqualToString:@"xdnote-sparkle-fixture.invalid"]);
}
- (void)updater:(SPUUpdater *)__unused updater willDownloadUpdate:(SUAppcastItem *)__unused item withRequest:(NSMutableURLRequest *)__unused request {
    self.archiveStarted = YES;
}
- (void)updater:(SPUUpdater *)__unused updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)__unused checkType error:(NSError *)error {
    NSMutableArray *chain = [NSMutableArray array];
    for (NSError *failure = error; failure != nil && chain.count < 8; failure = failure.userInfo[NSUnderlyingErrorKey]) {
        [chain addObject:@{@"code": @(failure.code), @"domain": failure.domain}];
    }
    report(@{@"error": @(error.code), @"errorChain": chain, @"archiveStarted": @(self.archiveStarted)}, 0);
}
@end

// Start the persistent downloader through Sparkle's public update flow. The
// fixture must fail on the network, before receiving data or extracting it.
@interface ProbeArchiveDriver : NSObject <SPUUserDriver>
@end
@implementation ProbeArchiveDriver
- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest *)__unused request reply:(void (^)(SUUpdatePermissionResponse *))reply {
    reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:NO sendSystemProfile:NO]);
}
- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {
    (void)cancellation;
}
- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem *)__unused item state:(SPUUserUpdateState *)__unused state reply:(void (^)(SPUUserUpdateChoice))reply {
    reply(SPUUserUpdateChoiceInstall);
}
- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
    (void)cancellation;
}
- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)__unused length {}
- (void)showDownloadDidReceiveDataOfLength:(uint64_t)__unused length {
    report(@{@"failed": @"unexpected-archive-data"}, 2);
}
- (void)showDownloadDidStartExtractingUpdate {
    report(@{@"failed": @"unexpected-extraction"}, 2);
}
- (void)showExtractionReceivedProgress:(double)__unused progress {}
- (void)showReadyToInstallAndRelaunch:(void (^ __unused)(SPUUserUpdateChoice))reply {
    report(@{@"failed": @"unexpected-installation"}, 2);
}
- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)__unused terminated retryTerminatingApplication:(void (^ __unused)(void))retry {
    report(@{@"failed": @"unexpected-installation"}, 2);
}
- (void)showUpdateInstalledAndRelaunched:(BOOL)__unused relaunched acknowledgement:(void (^ __unused)(void))acknowledgement {
    report(@{@"failed": @"unexpected-installation"}, 2);
}
- (void)dismissUpdateInstallation {}
- (void)showUpdateNotFoundWithError:(NSError *)__unused error acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}
- (void)showUpdaterError:(NSError *)__unused error acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}
- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData *)__unused data {}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError *)__unused error {}
@end

static void check(BOOL valid, NSString *name) {
    if (!valid) report(@{@"ok": @NO, @"failed": name}, 2);
    [checks addObject:name];
}
static SPUXdnoteDownloadProxyMode mode(NSString *name) {
    if ([name isEqualToString:@"system"]) return SPUXdnoteDownloadProxySystem;
    if ([name isEqualToString:@"direct"]) return SPUXdnoteDownloadProxyDirect;
    if ([name isEqualToString:@"custom"]) return SPUXdnoteDownloadProxyCustom;
    return (SPUXdnoteDownloadProxyMode)999;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        checks = [NSMutableArray array];
        if (argc == 2 && strcmp(argv[1], "policy") == 0) {
            NSError *error = nil;
            NSURLSessionConfiguration *system = SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxySystem, nil, &error);
            check(system != nil && error == nil && system.connectionProxyDictionary == nil, @"system-policy");
            check(system.HTTPCookieStorage == nil && system.URLCredentialStorage == nil && system.URLCache == nil &&
                  !system.HTTPShouldSetCookies && system.HTTPCookieAcceptPolicy == NSHTTPCookieAcceptPolicyNever, @"isolated-session");
            check(system.TLSMinimumSupportedProtocolVersion == tls_protocol_version_TLSv12, @"tls-minimum");
            NSURLSessionConfiguration *direct = SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxyDirect, nil, &error);
            NSArray *keys = @[(__bridge NSString *)kCFNetworkProxiesHTTPEnable, (__bridge NSString *)kCFNetworkProxiesHTTPSEnable,
                (__bridge NSString *)kCFNetworkProxiesSOCKSEnable, (__bridge NSString *)kCFNetworkProxiesFTPEnable,
                (__bridge NSString *)kCFNetworkProxiesProxyAutoConfigEnable, (__bridge NSString *)kCFNetworkProxiesProxyAutoDiscoveryEnable];
            check(direct != nil && direct.connectionProxyDictionary.count == keys.count, @"explicit-direct-policy");
            for (NSString *key in keys) check([direct.connectionProxyDictionary[key] isEqual:@0], key);
            for (NSString *text in @[@"http://user:secret@127.0.0.1:8000", @"https://127.0.0.1:8000", @"http://127.0.0.1",
                  @"http://127.0.0.1:0", @"http://127.0.0.1:65536", @"http://127.0.0.1:8000/path", @"http://127.0.0.1:8000?q=1",
                  @"socks5://127.0.0.1:8000#fragment"]) {
                error = nil;
                check(SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxyCustom, [NSURL URLWithString:text], &error) == nil && error != nil, @"invalid-proxy-rejected");
            }
            check(SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxyCustom, nil, nil) == nil, @"custom-needs-proxy");
            check(SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxyDirect, [NSURL URLWithString:@"http://127.0.0.1:8000"], nil) == nil, @"direct-rejects-proxy");
            check(SPUXdnoteCreateDownloadConfiguration((SPUXdnoteDownloadProxyMode)999, nil, nil) == nil, @"invalid-mode");
            if (@available(macOS 14.0, *)) {
                for (NSString *text in @[@"http://127.0.0.1:8000", @"socks5://127.0.0.1:8000", @"http://[::1]:8000"]) {
                    NSURLSessionConfiguration *custom = SPUXdnoteCreateDownloadConfiguration(SPUXdnoteDownloadProxyCustom, [NSURL URLWithString:text], nil);
                    check(custom.proxyConfigurations.count == 1 && !nw_proxy_config_get_failover_allowed(custom.proxyConfigurations[0]), @"proxy-no-failover");
                }
            }
            // No event-loop iteration or check call: start's queued cycle never
            // runs. Fixture Info.plist also disables automatic checks/downloads.
            SPUUpdater *updater = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle applicationBundle:NSBundle.mainBundle userDriver:(id<SPUUserDriver>)[NSObject new] delegate:nil];
            check([updater xdnote_setDownloadProxyMode:SPUXdnoteDownloadProxyDirect proxyURL:nil error:nil], @"configure-before-start");
            __block BOOL rejected = NO;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                rejected = ![updater xdnote_setDownloadProxyMode:SPUXdnoteDownloadProxySystem proxyURL:nil error:nil];
                dispatch_semaphore_signal(done);
            });
            check(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0 && rejected, @"main-thread-only");
            error = nil;
            check([updater startUpdater:&error], @"start-with-auto-checks-disabled");
            check(![updater xdnote_setDownloadProxyMode:SPUXdnoteDownloadProxySystem proxyURL:nil error:nil], @"immutable-after-start");
            report(@{@"ok": @YES, @"scope": @"native configuration only; no update check or installation", @"checks": checks}, 0);
        }
        if (argc == 3 && (strcmp(argv[1], "appcast") == 0 || strcmp(argv[1], "archive") == 0)) {
            BOOL archive = strcmp(argv[1], "archive") == 0;
            NSURL *url = [NSURL URLWithString:@(argv[2])];
            BOOL loopback = [url.host isEqualToString:@"127.0.0.1"];
            BOOL fixtureHost = [url.host isEqualToString:@"xdnote-sparkle-fixture.invalid"];
            BOOL https = [url.scheme isEqualToString:@"https"];
            check(url.port.intValue > 0 && (archive ? loopback && [url.scheme isEqualToString:@"http"] :
                  (loopback && [url.scheme isEqualToString:@"http"]) || (https && (loopback || fixtureHost))),
                  @"owned-appcast-fixture");
            ProbeUpdaterObserver *observer = [ProbeUpdaterObserver new];
            observer.feed = url;
            id userDriver = archive ? [ProbeArchiveDriver new] : [NSObject new];
            SPUUpdater *updater = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle applicationBundle:NSBundle.mainBundle userDriver:userDriver delegate:observer];
            check([updater xdnote_setDownloadProxyMode:SPUXdnoteDownloadProxySystem proxyURL:nil error:nil], @"system-download-mode");
            check([updater startUpdater:nil], @"start-isolated-updater");
            if (archive) {
                [updater checkForUpdates];
            } else {
                [updater checkForUpdateInformation];
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                (void)observer.feed;
                (void)updater.sessionInProgress;
                report(@{@"failed": @"sparkle-download-timeout"}, 2);
            });
            [[NSRunLoop mainRunLoop] run];
        }
        if (argc == 3 && strcmp(argv[1], "system-pac") == 0) {
            NSDictionary *proxies = CFBridgingRelease(CFNetworkCopySystemProxySettings());
            NSString *enableKey = (__bridge NSString *)kCFNetworkProxiesProxyAutoConfigEnable;
            NSString *urlKey = (__bridge NSString *)kCFNetworkProxiesProxyAutoConfigURLString;
            BOOL match = [proxies[enableKey] boolValue] && [proxies[urlKey] isEqualToString:@(argv[2])];
            report(@{@"matches": @(match)}, 0);
        }
        if (argc >= 4 && argc <= 5 && strcmp(argv[1], "request") == 0) {
            NSURL *url = [NSURL URLWithString:@(argv[3])];
            NSURL *proxy = argc == 5 ? [NSURL URLWithString:@(argv[4])] : nil;
            check([url.scheme isEqualToString:@"https"] && url.port.intValue > 0 &&
                  ([url.host isEqualToString:@"127.0.0.1"] || [url.host isEqualToString:@"xdnote-sparkle-fixture.invalid"]), @"fixture-target-only");
            check(proxy == nil || [proxy.host isEqualToString:@"127.0.0.1"], @"loopback-proxy-only");
            NSError *error = nil;
            NSURLSessionConfiguration *configuration = SPUXdnoteCreateDownloadConfiguration(mode(@(argv[2])), proxy, &error);
            check(configuration != nil, @"valid-request-policy");
            configuration.timeoutIntervalForRequest = 3;
            configuration.timeoutIntervalForResource = 5;
            ProbeTrustObserver *observer = [ProbeTrustObserver new];
            NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration delegate:observer delegateQueue:nil];
            [[session dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *failure) {
                (void)response;
                report(@{@"ok": @YES, @"error": @(failure.code), @"errorDomain": failure.domain ?: @"", @"receivedBytes": @(data.length), @"sawUntrustedServerTrust": @(observer.sawUntrustedServerTrust), @"scope": @"loopback native TLS/proxy; no updater"}, 0);
            }] resume];
            dispatch_main();
        }
        report(@{@"ok": @NO, @"failed": @"invalid-probe-command"}, 2);
    }
}
