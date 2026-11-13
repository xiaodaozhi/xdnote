#include "RecoveryStore.h"
#include "DocumentSession.h"
#include "Utf8Text.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtConcurrentRun>
namespace xdnote {
namespace {
constexpr qsizetype limit=32*1024*1024;
QString writeState(const QString &path,const QByteArray &bytes){
    if(path.isEmpty())return {};
    if(QFileInfo(path).isSymLink()||!QDir().mkpath(QFileInfo(path).absolutePath()))return QStringLiteral("恢复目录不可写，未保存原文仍保留在窗口中");
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)||file.write(bytes)!=bytes.size()||!file.commit())return QStringLiteral("无法保存恢复状态：")+file.errorString();
    QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner);return {};
}
}
bool RecoveryStore::validate(const QJsonObject &state){
    if(state.value("schemaVersion")!=QJsonValue(1))return false;
    for(const auto *key:{"documents","drafts"}){
        if(!state.value(key).isArray()||state.value(key).toArray().size()>128)return false;
        for(const auto &value:state.value(key).toArray()){
            if(!value.isObject())return false;const auto item=value.toObject();if(QUuid(item.value("id").toString()).isNull()||!item.value("path").isString()||item.value("path").toString().size()>4096||!item.value("root").isString()||item.value("root").toString().size()>4096)return false;
            if(item.contains("content")){if(!item.value("content").isString())return false;const auto encoded=item.value("content").toString().toLatin1();if(encoded.size()>DocumentSession::AdmissionSourceLimit*4/3+4)return false;const auto bytes=QByteArray::fromBase64(encoded);bool valid=false;decodeUtf8Literal(bytes,&valid);if(!valid||bytes.size()>DocumentSession::AdmissionSourceLimit||bytes.toBase64()!=encoded)return false;}
            else if(QString::fromLatin1(key)=="drafts")return false;
        }
    }return true;
}
RecoveryStore::RecoveryStore(QString path,QObject *parent):QObject(parent),m_path(std::move(path)){
    m_state={{"schemaVersion",1},{"documents",QJsonArray{}},{"drafts",QJsonArray{}}};
    if(!m_path.isEmpty()&&QFileInfo::exists(m_path)){QFile file(m_path);if(QFileInfo(m_path).isSymLink()||!file.open(QIODevice::ReadOnly)||file.size()>limit){m_valid=false;m_error=QStringLiteral("恢复记录不可读，原记录已保留");}else{const auto parsed=QJsonDocument::fromJson(file.readAll());if(!parsed.isObject()||!validate(parsed.object())){m_valid=false;m_error=QStringLiteral("恢复记录损坏或版本不支持，原记录已保留");}else m_state=parsed.object();}}
    connect(&m_writer,&QFutureWatcher<QString>::finished,this,[this]{m_active=false;const auto error=m_writer.result();m_error=error;if(!error.isEmpty())emit failed(error);else emit persisted();if(!m_pending.isEmpty())startWrite();});
}
RecoveryStore::~RecoveryStore(){flush();}
void RecoveryStore::save(QJsonObject state){
    if(!m_valid){emit failed(m_error);return;}if(!validate(state)){m_error=QStringLiteral("恢复状态无效，原记录已保留");emit failed(m_error);return;}const auto bytes=QJsonDocument(state).toJson(QJsonDocument::Compact);if(bytes.size()>limit){m_error=QStringLiteral("恢复草稿达到 32 MiB 上限，原记录已保留");emit failed(m_error);return;}
    m_state=std::move(state);if(m_path.isEmpty())return;if(bytes==m_lastQueued&&m_error.isEmpty())return;m_lastQueued=bytes;m_pending=bytes;if(!m_active)startWrite();
}
void RecoveryStore::startWrite(){if(m_pending.isEmpty())return;const auto bytes=m_pending;const auto path=m_path;m_pending.clear();m_active=true;m_writer.setFuture(QtConcurrent::run([path,bytes]{return writeState(path,bytes);}));}
bool RecoveryStore::flush(){if(m_active){m_writer.waitForFinished();m_error=m_writer.result();m_active=false;}if(!m_pending.isEmpty()){const auto bytes=m_pending;m_pending.clear();m_error=writeState(m_path,bytes);}return m_valid&&m_error.isEmpty();}
}
