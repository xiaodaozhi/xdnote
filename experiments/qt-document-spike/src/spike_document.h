#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <functional>

// Experimental fixture code, not the product's file API.
class SpikeFile final
{
public:
    enum class SaveOutcome { Saved, Conflict, FailedBeforeCommit, CommitUncertain };

    bool open(const QString &path, QString *text, QString *error);
    SaveOutcome save(const QString &text, const std::function<void()> &beforeFinalCheck,
                     QString *error);

private:
    SaveOutcome checkBaseline(QString *error) const;
    QString m_path;
    QByteArray m_baseline;
    bool m_bom = false;
    bool m_crlf = false;
};

class SpikeSession final
{
public:
    bool open(const QString &path, QString *error);
    SpikeFile::SaveOutcome save(const std::function<void()> &beforeFinalCheck,
                                QString *error);
    bool isDirty() const;
    QTextDocument *document();

private:
    SpikeFile m_file;
    QTextDocument m_document;
    QString m_savedText;
};

class RestrictedPreview final : public QTextDocument
{
public:
    void render(const QString &markdown);
    QStringList requestedResources() const;

protected:
    QVariant loadResource(int type, const QUrl &name) override;

private:
    QStringList m_requestedResources;
};
