#include "SystemIntegration.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif
namespace xdnote {
QList<RegistrationValue> associationValues(const QString &exe){
    if(exe.isEmpty()||exe.contains('"')||exe.contains('\n')||exe.contains('\r'))return {};
    const QString command='"'+QDir::toNativeSeparators(exe)+QStringLiteral("\" \"%1\"");const QString classes="Software\\Classes\\";
    return {{classes+"xdnote.Markdown",{},QStringLiteral("Markdown 文档")},{classes+"xdnote.Markdown\\DefaultIcon",{},'"'+QDir::toNativeSeparators(exe)+"\",0"},{classes+"xdnote.Markdown\\shell\\open\\command",{},command},
        {classes+".md\\OpenWithProgids","xdnote.Markdown",{}},{classes+".markdown\\OpenWithProgids","xdnote.Markdown",{}},{classes+"Applications\\xdnote.exe\\shell\\open\\command",{},command},
        {classes+"Applications\\xdnote.exe\\SupportedTypes",".md",{}},{classes+"Applications\\xdnote.exe\\SupportedTypes",".markdown",{}},
        {"Software\\xdnote\\Capabilities","ApplicationName","xdnote"},{"Software\\xdnote\\Capabilities","ApplicationDescription",QStringLiteral("Markdown 阅读与编辑")},{"Software\\xdnote\\Capabilities\\FileAssociations",".md","xdnote.Markdown"},{"Software\\xdnote\\Capabilities\\FileAssociations",".markdown","xdnote.Markdown"},{"Software\\RegisteredApplications","xdnote","Software\\xdnote\\Capabilities"}};
}
QString associationStatus(){
#ifdef Q_OS_WIN
    const auto values=associationValues(QCoreApplication::applicationFilePath());bool registered=!values.isEmpty();for(const auto &entry:values){QSettings settings("HKEY_CURRENT_USER\\"+entry.path,QSettings::NativeFormat);const auto name=entry.name.isEmpty()?QString("."):entry.name;registered=registered&&settings.contains(name)&&settings.value(name).toString()==entry.value;}return registered?QStringLiteral("此副本已注册为可选打开方式；默认应用由系统管理"):QStringLiteral("此副本尚未注册打开方式");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("应用包声明 Markdown 文档类型；请在 Finder 的显示简介中选择默认打开方式");
#else
    return QStringLiteral("可注册到当前用户的应用列表；默认应用在文件管理器中选择");
#endif
}
QString registerFileAssociations(){
#ifdef Q_OS_WIN
    const auto entries=associationValues(QCoreApplication::applicationFilePath());if(entries.isEmpty())return QStringLiteral("程序路径无法安全注册");
    for(const auto &entry:entries){HKEY key=nullptr;const auto path=entry.path.toStdWString(),name=entry.name.toStdWString(),value=entry.value.toStdWString();auto error=RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);if(error==ERROR_SUCCESS){error=RegSetValueExW(key,name.c_str(),0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));RegCloseKey(key);}if(error!=ERROR_SUCCESS)return QStringLiteral("注册未完全完成（系统错误 %1），可重试；默认应用未改变").arg(error);}
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);return associationStatus();
#elif defined(Q_OS_LINUX)
    auto exe=QCoreApplication::applicationFilePath();if(exe.contains('\n')||exe.contains('\r')||exe.contains('\\')||exe.contains('%'))return QStringLiteral("程序路径暂不支持应用列表注册");exe.replace("\"","\\\"");exe.replace("$","\\$");exe.replace("`","\\`");const auto root=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/applications";if(!QDir().mkpath(root))return QStringLiteral("无法创建用户应用目录");const auto path=root+"/io.github.xiaodaozhi.xdnote.desktop";if(QFileInfo(path).isSymLink())return QStringLiteral("已有入口是链接，已保留；请检查后重试");
    QFile existing(path);if(existing.exists()){if(!existing.open(QIODevice::ReadOnly)||existing.readAll()!=QString("[Desktop Entry]\nType=Application\nName=xdnote\nExec=\"%1\" %F\nTerminal=false\nMimeType=text/markdown;text/x-markdown;\n").arg(exe).toUtf8())return QStringLiteral("已有应用入口来自其他副本，已保留");return QStringLiteral("已注册此副本；请在文件管理器选择默认打开方式");}
    QFile file(path);const auto bytes=QString("[Desktop Entry]\nType=Application\nName=xdnote\nExec=\"%1\" %F\nTerminal=false\nMimeType=text/markdown;text/x-markdown;\n").arg(exe).toUtf8();if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)||file.write(bytes)!=bytes.size())return QStringLiteral("无法写入用户应用入口");return QStringLiteral("已注册此副本；请在文件管理器选择默认打开方式");
#else
    return QStringLiteral("请将应用包放入 Applications 并从 Finder 打开；文档类型由应用包声明，不强制修改系统默认值");
#endif
}
bool openDefaultApplicationSettings(){
#ifdef Q_OS_WIN
    return QDesktopServices::openUrl(QUrl("ms-settings:defaultapps"));
#else
    return false;
#endif
}
}
