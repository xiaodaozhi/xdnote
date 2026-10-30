#pragma once

#include <QString>

// Test-only primitive: does not implement a product save or a conditional swap.
bool replaceAndKeepDisplaced(const QString &target, const QString &prepared,
                             const QString &windowsBackup, QString *displaced,
                             QString *error);
