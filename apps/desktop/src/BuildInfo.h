#pragma once
#include <QString>
namespace xdnote::BuildInfo {
QString version();
QString revision();
bool dirty();
// Packaged or failed package identity queries belong to Store, fail closed.
bool packaged();
}
