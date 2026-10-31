#pragma once

#include <QByteArray>
#include <QStringList>
#include <optional>

// A conservative lexical candidate only: success does not authorize a read.
// Returned components are decoded once and must not be decoded as URLs again.
std::optional<QStringList> candidateImageReference(const QByteArray &encodedReference);
