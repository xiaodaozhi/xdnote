#pragma once
#include <QString>
namespace xdnote::BuildInfo {
QString version();
QString revision();
QString channel();
QString configuration();
int buildNumber();
bool release();
bool dirty();
// Packaged or failed package identity queries belong to Store, fail closed.
bool packaged();
QString settingsScope(const QString &base);
}
