#include "DocumentExport.h"
#include "MarkdownHeadings.h"
#include "FileService.h"
#include "UiFonts.h"
#include <QSaveFile>
#include <QFileInfo>
#include <QPdfWriter>
#include <QPageLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QUrl>
#include <QImage>

namespace xdnote {
namespace {
using Kind=NativePreviewModel::Kind;
constexpr qsizetype MaximumHtml=64*1024*1024;
QString escaped(QByteArrayView value){return QString::fromUtf8(value).toHtmlEscaped();}
bool safeLink(const QString &target){if(target.isEmpty()||target.startsWith("//")||target.contains('\\'))return false;for(const auto ch:target)if(ch.category()==QChar::Other_Control)return false;const QUrl url(target);return url.isValid()&&(url.scheme()=="https"||url.scheme()=="http"||url.scheme()=="mailto"||(url.isRelative()&&!target.contains(':')));}
class ExportDocument final:public QTextDocument {
    QVariant loadResource(int type,const QUrl&url)override{
        const auto text=url.toString();const QString prefix="data:image/png;base64,";
        if(type==QTextDocument::ImageResource&&text.startsWith(prefix)&&text.size()<MaximumHtml){QImage image;image.loadFromData(QByteArray::fromBase64(text.mid(prefix.size()).toLatin1()),"PNG");return image;}
        return {};
    }
};
}
DocumentExportData prepareDocumentExport(const DocumentSnapshot&snapshot,const QString&title,
    const std::shared_ptr<WorkspaceResourceReader>&root,const QString&path,const QString&identity,
    const AuthorizedPreviewImages&authorized,const QJsonObject&reader,const PreviewCancellation&cancel){
    DocumentExportData result;result.document=snapshot.documentId;result.revision=snapshot.revision;
    if(snapshot.result!=EditResult::Applied){result.error=QStringLiteral("当前文档正在处理，请稍后导出。");return result;}
    QJsonObject metrics;auto analysis=PreviewAnalysis::get(snapshot,cancel,metrics,&result.error);if(!analysis)return result;
    const auto images=preparePreviewImages(snapshot,analysis->references,root,path,identity,authorized,cancel,std::make_shared<PreviewImageCache>());
    if(previewCancelled(cancel)){result.error=QStringLiteral("已取消导出。");return result;}
    const auto headings=analyzeMarkdownHeadings(snapshot);const auto&model=analysis->model;
    struct Frame{quint32 end;QString close;Kind kind;};QVector<Frame> stack;QString body;int headingIndex=0;
    for(quint32 i=0;i<=model.nodeCount();++i){while(!stack.isEmpty()&&stack.last().end==i)body+=stack.takeLast().close;if(i==model.nodeCount())break;if((i&255)==0&&previewCancelled(cancel)){result.error=QStringLiteral("已取消导出。");return result;}
        const auto n=model.node(i);QString open,close;switch(n.kind){
        case Kind::Paragraph:case Kind::LiteralBlock:open="<p>";close="</p>\n";break;
        case Kind::Heading:{const auto level=qBound(1,int(n.detail),6);const auto id=headings.success&&headingIndex<headings.headings.size()?headings.headings[headingIndex].id:"heading-"+QString::number(i);++headingIndex;open=QString("<h%1 id=\"%2\">").arg(level).arg(id.toHtmlEscaped());close=QString("</h%1>\n").arg(level);break;}
        case Kind::BlockQuote:open="<blockquote>";close="</blockquote>\n";break;
        case Kind::CodeBlock:open="<pre><code>";close="</code></pre>\n";break;
        case Kind::BulletList:open="<ul>";close="</ul>\n";break;
        case Kind::OrderedList:open=QString("<ol start=\"%1\">").arg(n.detail);close="</ol>\n";break;
        case Kind::Item:open="<li>";close="</li>\n";break;
        case Kind::Table:open="<table>";close="</table>\n";break;
        case Kind::TableHead:case Kind::TableRow:open="<tr>";close="</tr>\n";break;
        case Kind::TableCell:{bool header=false;for(const auto&f:stack)header|=f.kind==Kind::TableHead;open=header?"<th>":"<td>";close=header?"</th>":"</td>";break;}
        case Kind::Emphasis:open="<em>";close="</em>";break;
        case Kind::Strong:open="<strong>";close="</strong>";break;
        case Kind::Strikethrough:open="<s>";close="</s>";break;
        case Kind::Link:{const auto target=QString::fromUtf8(model.text(n));open=safeLink(target)?"<a href=\""+target.toHtmlEscaped()+"\">":"<span>";close=safeLink(target)?"</a>":"</span>";break;}
        case Kind::Image:{QString alt;for(quint32 j=i+1;j<n.subtreeEnd;++j){const auto child=model.node(j);if(child.kind==Kind::Text||child.kind==Kind::InlineCode)alt+=QString::fromUtf8(model.text(child));}auto image=images.png.constFind(int(n.detail));if(image!=images.png.cend())body+="<img alt=\""+alt.toHtmlEscaped()+"\" src=\"data:image/png;base64,"+QString::fromLatin1(image->toBase64())+"\">";else body+="<span class=\"image-placeholder\">图片："+alt.toHtmlEscaped()+"</span>";i=n.subtreeEnd-1;break;}
        case Kind::InlineCode:body+="<code>"+escaped(model.text(n))+"</code>";break;
        case Kind::Text:case Kind::LiteralHtml:body+=escaped(model.text(n));break;
        case Kind::SoftBreak:body+=' ';break;
        case Kind::HardBreak:body+="<br>";break;
        case Kind::Rule:body+="<hr>";break;
        case Kind::TaskMarker:body+=n.detail?QStringLiteral("<span class=\"task\">☑</span> "):QStringLiteral("<span class=\"task\">☐</span> ");break;
        }
        body+=open;if(!close.isEmpty())stack.append({n.subtreeEnd,close,n.kind});
        if(body.size()*2>MaximumHtml){result.error=QStringLiteral("导出内容超过当前大小限制，原文保持不变。");return result;}
    }
    QString family=reader["fontFamily"].toString();if(family.isEmpty())family="sans-serif";family.replace('\\',"\\\\");family.replace('\'',"\\'");family.remove('\n');family.remove('\r');
    const QString css=QString("body{font-family:'%1',sans-serif;font-size:%2px;line-height:%3;color:#1F2937;background:white;max-width:%4px;margin:40px auto;padding:0 24px}h1,h2,h3{line-height:1.4}h1{font-size:28px}h2{font-size:22px}h3{font-size:18px}blockquote{border-left:3px solid #AAB4C0;background:#F3F5F8;margin-left:0;padding:8px 16px;color:#667085}pre{background:#F1F3F5;padding:16px;white-space:pre-wrap}code{font-family:monospace}table{border-collapse:collapse;width:100%}th,td{border:1px solid #DDE2E8;padding:6px}th{background:#F4F5F7}a{color:#245CB8}img{max-width:100%;height:auto}.image-placeholder{background:#F4F5F7;color:#667085}.task{vertical-align:middle}")
        .arg(family.toHtmlEscaped()).arg(reader["fontSize"].toInt(16)).arg(reader["lineHeight"].toDouble(1.8)).arg(reader["maxWidth"].toInt(760));
    result.html=("<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>"+title.toHtmlEscaped()+"</title><style>"+css+"</style></head><body>"+body+"</body></html>").toUtf8();
    if(result.html.size()>MaximumHtml){result.html.clear();result.error=QStringLiteral("导出内容超过当前大小限制。");}return result;
}
bool writeDocumentExport(const DocumentExportData&data,const QString&destination,bool pdf,const QString&sourcePath,const QString&sourceIdentity,QString*error){
    auto fail=[&](const QString&message){if(error)*error=message;return false;};if(data.html.isEmpty()||!data.error.isEmpty())return fail(data.error.isEmpty()?QStringLiteral("没有可导出的内容。"):data.error);
    const QFileInfo target(destination),source(sourcePath);if(!target.isAbsolute()||target.suffix().compare(pdf?"pdf":"html",Qt::CaseInsensitive)!=0)return fail(QStringLiteral("请选择正确的导出文件类型。"));
    if(target.absoluteFilePath().compare(source.absoluteFilePath(),
#ifdef Q_OS_WIN
        Qt::CaseInsensitive
#else
        Qt::CaseSensitive
#endif
    )==0)return fail(QStringLiteral("导出不能覆盖当前 Markdown 文档。"));
    if(target.exists()){const auto probe=FileService::probe(target.absoluteFilePath());if(!probe.file)return fail(probe.problem.description());if(probe.file->identity()==sourceIdentity)return fail(QStringLiteral("导出不能覆盖原文或它的硬链接。"));}
    QSaveFile file(target.absoluteFilePath());file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly))return fail(file.errorString());
    if(!pdf){if(file.write(data.html)!=data.html.size())return fail(file.errorString());}
    else {QPdfWriter writer(&file);writer.setResolution(96);writer.setPageSize(QPageSize(QPageSize::A4));writer.setPageMargins(QMarginsF(15,15,15,15));writer.setTitle(source.completeBaseName());writer.setCreator("xdnote");ExportDocument document;document.setDefaultFont(uiFont(16));auto option=document.defaultTextOption();option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);document.setDefaultTextOption(option);document.setHtml(QString::fromUtf8(data.html));document.setPageSize(QSizeF(writer.width(),writer.height()));for(auto block=document.begin();block.isValid();block=block.next()){QTextCursor cursor(block);auto format=block.blockFormat();format.setNonBreakableLines(false);cursor.setBlockFormat(format);}if(document.pageCount()>1000)return fail(QStringLiteral("PDF 页数超过当前限制，请缩小导出范围。"));document.print(&writer);}
    if(file.pos()<8)return fail(QStringLiteral("未能生成导出文件。"));
    if(!file.commit())return fail(file.errorString());return true;
}
}
