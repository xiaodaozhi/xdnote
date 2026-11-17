#include "BuildInfo.h"
#include "BuildVersion.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <appmodel.h>
#endif
namespace xdnote::BuildInfo {
QString version() { return QStringLiteral(XDNOTE_VERSION); }
QString revision() { return QStringLiteral(XDNOTE_REVISION); }
bool dirty() { return XDNOTE_DIRTY; }
bool packaged() {
#ifdef Q_OS_WIN
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
#else
    return false;
#endif
}
}
