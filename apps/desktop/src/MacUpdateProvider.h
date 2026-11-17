// apps/desktop/MacUpdateProvider.h

#pragma once
#include <QJsonObject>
#include <QString>
#include <functional>
namespace xdnote { class UpdateController; }
class QObject;
QString macUpdateInstallRoot();
struct MacUpdateTransport {
    QString proxyMode = QStringLiteral("system");
    QString proxyAddress;
    QString mirror;
};
void startMacUpdate(xdnote::UpdateController *owner, const QJsonObject &candidate,
                    std::function<void(QString)> prepared,
                    const MacUpdateTransport &transport = {});
bool commitMacUpdate();
bool macUpdateInstallationCommitted();
// Runs synchronously after actual native commitment, before invoking Sparkle's
// install reply. The context prevents invoking a callback after its owner dies.
void setMacUpdateCommitCallback(QObject *context, std::function<void()> callback);
void cancelMacUpdate();
// Returns true when normal application exit must wait for Sparkle to finish
// cancelling its active cycle. A committed installation owns its normal quit.
bool deferMacUpdateQuit(std::function<void()> quit);
QJsonObject macUpdateExitStatus();
#ifdef XDNOTE_UPDATE_TESTS
bool macUpdateLifecycleProbe(xdnote::UpdateController *owner);
#endif
