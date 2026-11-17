#include "Preferences.h"
#include "Shortcuts.h"
#include "VisualTokens.h"
#include "UpdateNetworkSettings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <algorithm>

namespace xdnote {
namespace {
QJsonObject preserveUnknown(QJsonObject next, const QJsonObject &stored, const QJsonObject &known) {
    for (auto field = stored.begin(); field != stored.end(); ++field) {
        if (!known.contains(field.key())) next[field.key()] = field.value();
        else if (known[field.key()].isObject() && field.value().isObject())
            next[field.key()] = preserveUnknown(next[field.key()].toObject(), field.value().toObject(), known[field.key()].toObject());
    }
    return next;
}
}
Preferences::Preferences(QObject *parent, QString path) : QObject(parent), m_path(std::move(path)) {
    const auto fonts = visualTokens()["typography"].toObject();
    const auto source = fonts["source"].toObject(), reader = fonts["reader"].toObject();
    m_defaults = {{"schemaVersion", 1}, {"appearance", QJsonObject{{"theme", "system"}, {"accent", "graphite"}}},
        {"desktop", QJsonObject{{"externalOpenTarget", "standalone"}, {"standaloneDefaultMode", "reading"}}},
        {"editor", QJsonObject{{"fontFamily", ""}, {"fontSize", source["size"]}, {"lineHeight", source["lineHeight"]},
            {"wordWrap", true}, {"lineNumbers", true}, {"indentWidth", 2}, {"useSpaces", true},
            {"autoSave", QJsonObject{{"enabled", false}, {"delayMs", 1500}}}}},
        {"reader", QJsonObject{{"fontFamily", ""}, {"fontSize", reader["size"]}, {"lineHeight", reader["lineHeight"]},
            {"maxWidth", reader["maxContentWidth"]}}}, {"files", QJsonObject{{"confirmUnsavedClose", true}}}};
    m_defaults["shortcuts"]=shortcutDefaults();
    m_defaults["updates"] = QJsonObject{{"proxyMode", "system"}, {"proxyAddress", ""}, {"mirror", ""}};
    m_values = m_defaults;
    if (m_path.isEmpty() || !QFileInfo::exists(m_path)) return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        m_valid = false; m_error = QStringLiteral("无法读取设置，原配置已保留"); return;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !document.object()["schemaVersion"].isDouble() || document.object()["schemaVersion"].toDouble() != 1) {
        m_valid = false; m_error = QStringLiteral("设置格式无效或版本不支持，原配置已保留"); return;
    }
    m_values = document.object();
    bool fallback = false;QString shortcutWarning;
    for (auto section = m_defaults.begin(); section != m_defaults.end(); ++section) {
        if (!section.value().isObject()) continue;
        // Non-const operator[] inserts a null entry for a missing key. Read
        // without mutation so optional legacy fields stay distinguishable
        // from present, invalid values until defaults are applied below.
        const auto storedSection = m_values.value(section.key());
        fallback |= !storedSection.isUndefined() && !storedSection.isObject();
        if (section.key() == "updates" && !storedSection.isUndefined() && !storedSection.isObject()) {
            m_updateNetworkValid = false; continue;
        }
        auto loaded = storedSection.toObject();
        const auto defaults = section.value().toObject();
        for (auto field = defaults.begin(); field != defaults.end(); ++field) {
            const auto storedValue = loaded.value(field.key());
            if (section.key() == "editor" && field.key() == "autoSave") {
                fallback |= !storedValue.isUndefined() && !storedValue.isObject();
                auto autosave = storedValue.toObject();
                for (const auto &name : {QString("enabled"), QString("delayMs")}) {
                    const auto saved = autosave.value(name);
                    if (!valid("editor", name == "enabled" ? "autoSaveEnabled" : "autoSaveDelayMs", saved)) {
                        fallback |= !saved.isUndefined(); autosave[name] = field.value().toObject().value(name);
                    }
                }
                loaded[field.key()] = autosave; continue;
            }
            if (!valid(section.key(), field.key(), storedValue)) {
                fallback |= !storedValue.isUndefined();
                if (section.key() == "updates" && !storedValue.isUndefined()) m_updateNetworkValid = false;
                else loaded[field.key()] = field.value();
            }
        }
        m_values[section.key()] = loaded;
    }
    const auto network = m_values.value("updates").toObject();
    m_updateNetworkValid &= UpdateNetwork::validConfiguration(network.value("proxyMode").toString(), network.value("proxyAddress").toString(), network.value("mirror").toString());
    if(!distinctShortcuts(m_values.value("shortcuts").toObject())){
        auto loaded=m_values.value("shortcuts").toObject();const auto supplied=document.object().value("shortcuts").toObject();QJsonObject accepted=shortcutDefaults();for(const auto &command:commandShortcuts())accepted[command.id]="";
        auto commands=commandShortcuts();std::stable_sort(commands.begin(),commands.end(),[&](const auto &left,const auto &right){return supplied.contains(left.id)&&!supplied.contains(right.id);});
        for(const auto &command:commands){auto trial=accepted;trial[command.id]=loaded.value(command.id);if(distinctShortcuts(trial))accepted=trial;else if(supplied.contains(command.id))fallback=true;else shortcutWarning=QStringLiteral("新增命令的默认快捷键与已有自定义绑定冲突，已暂时禁用新增绑定；原自定义设置保留。");}
        for(const auto &command:commands)loaded[command.id]=accepted.value(command.id);m_values["shortcuts"]=loaded;
    }
    if (!m_updateNetworkValid) m_error = QStringLiteral("更新网络设置无效，已暂停更新；请修正配置。");
    else if (fallback) m_error = QStringLiteral("部分设置值无效，已使用默认值；原配置保留至下一次成功保存。");else m_error=shortcutWarning;
}
QJsonValue Preferences::value(const QString &section, const QString &key) const {
    if (section == "updates") {
        const auto saved = m_values.value(section).toObject().value(key);
        return valid(section, key, saved) ? saved : m_defaults.value(section).toObject().value(key);
    }
    if (section == "editor" && (key == "autoSaveEnabled" || key == "autoSaveDelayMs")) return m_values[section].toObject()["autoSave"].toObject()[key == "autoSaveEnabled" ? "enabled" : "delayMs"];
    return m_values[section].toObject()[key];
}
bool Preferences::valid(const QString &section, const QString &key, const QJsonValue &value) const {
    const auto number = [&](double minimum, double maximum, bool integer = false) {
        const double n = value.toDouble(-1); return value.isDouble() && std::isfinite(n) && n >= minimum && n <= maximum && (!integer || std::floor(n) == n);
    };
    if(section=="shortcuts")return shortcutDefaults().contains(key)&&value.isString()&&validShortcut(value.toString());
    if (section == "updates") {
        if (key == "proxyMode") return value.isString() && QStringList{"system", "none", "custom"}.contains(value.toString());
        if (key == "proxyAddress") return value.isString() && (value.toString().isEmpty() || UpdateNetwork::validProxy(value.toString()));
        if (key == "mirror") return value.isString() && UpdateNetwork::validMirror(value.toString());
    }
    if (section == "appearance") {
        if (key == "theme") return value.isString() && QStringList{"system", "light", "dark"}.contains(value.toString());
        if (key == "accent") return value.isString() && visualTokens()["accents"].toObject().contains(value.toString());
    }
    if (section == "desktop") {
        if (key == "externalOpenTarget") return value.isString() && QStringList{"standalone", "workspace-tab"}.contains(value.toString());
        if (key == "standaloneDefaultMode") return value.isString() && QStringList{"reading", "source"}.contains(value.toString());
    }
    if (section == "editor" || section == "reader") {
        if (key == "fontFamily") { const auto name = value.toString(); return value.isString() && name.size() <= 128 && !name.contains(QChar::Null) && !name.contains('\n') && !name.contains('\r'); }
        if (key == "fontSize") return number(8, 40, true);
        if (key == "lineHeight") return number(1, 3);
    }
    if (section == "editor") {
        if (key == "autoSave") return value.isObject() && valid(section, "autoSaveEnabled", value.toObject()["enabled"]) && valid(section, "autoSaveDelayMs", value.toObject()["delayMs"]);
        if (key == "wordWrap" || key == "lineNumbers" || key == "useSpaces" || key == "autoSaveEnabled") return value.isBool();
        if (key == "indentWidth") return number(1, 8, true);
        if (key == "autoSaveDelayMs") return number(500, 30000, true);
    }
    if (section == "reader" && key == "maxWidth") return number(320, 1600, true);
    if (section == "files" && key == "confirmUnsavedClose") return value == QJsonValue(true);
    return false;
}
bool Preferences::fail(const QString &message) { m_error = message; emit writeFailed(message); return false; }
bool Preferences::setValues(const QString &section, const QJsonObject &patch) {
    m_retryWrite = false;
    if (!m_valid) return fail(m_error);
    const auto known = m_defaults[section].toObject();
    if (known.isEmpty() || patch.isEmpty()) return fail(QStringLiteral("设置项无效"));
    auto next = m_values; auto values = next[section].toObject();
    bool changed = false;
    for (auto field = patch.begin(); field != patch.end(); ++field) {
        if (section == "editor" && (field.key() == "autoSaveEnabled" || field.key() == "autoSaveDelayMs")) {
            if (!valid(section, field.key(), field.value())) return fail(QStringLiteral("自动保存选项或延时无效"));
            const auto key = field.key() == "autoSaveEnabled" ? "enabled" : "delayMs"; auto autosave = values["autoSave"].toObject();
            changed |= autosave[key] != field.value(); autosave[key] = field.value(); values["autoSave"] = autosave; continue;
        }
        if (!known.contains(field.key()) || !valid(section, field.key(), field.value())) return fail(QStringLiteral("设置选项或数值无效"));
        if (section == "editor" && field.key() == "autoSave") {
            auto autosave = values[field.key()].toObject(); const auto supplied = field.value().toObject();
            for (auto entry = supplied.begin(); entry != supplied.end(); ++entry) autosave[entry.key()] = entry.value();
            changed |= values[field.key()] != autosave; values[field.key()] = autosave; continue;
        }
        changed |= values[field.key()] != field.value(); values[field.key()] = field.value();
    }
    if(section=="shortcuts"&&!distinctShortcuts(values))return fail(QStringLiteral("快捷键与其他命令相同或存在前缀冲突，本次更改未生效"));
    if (section == "updates") {
        for (auto field = known.begin(); field != known.end(); ++field)
            if (!valid(section, field.key(), values.value(field.key()))) return fail(QStringLiteral("更新网络设置无效，本次更改未生效"));
        if (!UpdateNetwork::validConfiguration(values["proxyMode"].toString(), values["proxyAddress"].toString(), values["mirror"].toString()))
            return fail(QStringLiteral("自定义代理需要无凭据的 HTTP 或 SOCKS5 地址及端口"));
    }
    if (!changed) return true;
    next[section] = values; next["schemaVersion"] = 1;
    if (!m_path.isEmpty()) {
        m_failedSection = section; m_failedPatch = patch; m_retryWrite = true;
        if (!QDir().mkpath(QFileInfo(m_path).path())) return fail(QStringLiteral("无法创建设置目录，本次更改未生效"));
        if (QFileInfo::exists(m_path)) {
            QFile existing(m_path);
            if (!existing.open(QIODevice::ReadOnly) || existing.size() > 65536)
                return fail(QStringLiteral("无法读取当前设置，本次更改未生效；原配置已保留"));
            const auto stored = QJsonDocument::fromJson(existing.readAll());
            if (!stored.isObject() || stored.object()["schemaVersion"].toDouble(-1) != 1)
                return fail(QStringLiteral("当前设置格式无效，本次更改未生效；原配置已保留"));
            next = preserveUnknown(next, stored.object(), m_defaults);
        }
        const auto bytes = QJsonDocument(next).toJson();
        if (bytes.size() > 65536) return fail(QStringLiteral("设置文件过大，本次更改未生效"));
        QSaveFile file(m_path); file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return fail(QStringLiteral("无法保存设置，本次更改未生效：") + file.errorString());
    }
    m_values = next; if (section == "updates") m_updateNetworkValid = true;
    m_error = m_updateNetworkValid ? QString() : QStringLiteral("更新网络设置无效，已暂停更新；请修正配置。");
    m_retryWrite = false; m_failedSection.clear(); m_failedPatch = {}; emit this->changed(section); return true;
}
bool Preferences::retryWrite() { if (!m_retryWrite) return false; const auto section = m_failedSection; const auto patch = m_failedPatch; return setValues(section, patch); }
}
