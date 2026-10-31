#pragma once

#include <QString>
#include <QtGlobal>

// Creates only workspace/linked -> outside inside the test's own fixture.
// Returns ERROR_SUCCESS or the immediate Windows error; never targets user paths.
quint32 createFixtureJunction(const QString &fixtureRoot);
