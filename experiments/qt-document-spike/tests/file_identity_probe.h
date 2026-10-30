#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

// A test-only observation, not a persistent or universal document ID.
struct FixtureFileIdentity
{
    quint64 volume = 0;
    QByteArray object;

    bool operator==(const FixtureFileIdentity &other) const
    {
        return volume == other.volume && object == other.object;
    }
};

// Keeps the observed object open so the test can compare live handles. This
// does not bind a later pathname lookup or replacement to the same object.
class FixtureIdentityHandle final
{
public:
    FixtureIdentityHandle() = default;
    ~FixtureIdentityHandle();
    FixtureIdentityHandle(const FixtureIdentityHandle &) = delete;
    FixtureIdentityHandle &operator=(const FixtureIdentityHandle &) = delete;

    bool open(const QString &path, QString *error);
    bool identity(FixtureFileIdentity *value, QString *error) const;

private:
#ifdef Q_OS_WIN
    void *m_handle = nullptr;
#else
    int m_descriptor = -1;
#endif
};
