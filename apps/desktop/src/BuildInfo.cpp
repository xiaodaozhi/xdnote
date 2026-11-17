#include "BuildInfo.h"
#include <QCryptographicHash>
#include "BuildVersion.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <string>
#include <appmodel.h>
#endif
namespace xdnote::BuildInfo {
QString version() { return QStringLiteral(XDNOTE_VERSION); }
QString revision() { return QStringLiteral(XDNOTE_REVISION); }
QString channel() { return QStringLiteral(XDNOTE_CHANNEL); }
QString configuration() { return QStringLiteral(XDNOTE_CONFIGURATION); }
int buildNumber() { return XDNOTE_BUILD; }
bool release() { return !dirty() && buildNumber() > 0 && channel() != "development" && configuration() == "Release" && revision().size() == 40; }
bool dirty() { return XDNOTE_DIRTY; }
bool packaged() {
#ifdef Q_OS_WIN
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
#else
    return false;
#endif
}
QString settingsScope(const QString &base) {
    if (!packaged()) return base;
#ifdef Q_OS_WIN
    UINT32 length = 0;
    const auto status = GetCurrentPackageFamilyName(&length, nullptr);
    if (status == ERROR_INSUFFICIENT_BUFFER && length > 1 && length < 32768) {
        std::wstring family(length, L'\0');
        if (GetCurrentPackageFamilyName(&length, family.data()) == ERROR_SUCCESS) {
            const auto hash = QCryptographicHash::hash(QString::fromWCharArray(family.c_str()).toUtf8(), QCryptographicHash::Sha256).toHex();
            return base + "/store-" + QString::fromLatin1(hash.left(24));
        }
    }
#endif
    return base + "/store-unknown";
}

}
