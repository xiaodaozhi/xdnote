#include "ScintillaEditBase.h"

#include <QAccessible>
#include <QApplication>
#include <QDebug>
#include <QPlainTextEdit>
#include <QSet>

namespace {
struct Observation {
    int nodes = 0;
    int textInterfaces = 0;
    int editableTextInterfaces = 0;
    bool sampleInText = false;
    bool sampleInValue = false;
    bool complete = true;
};

void inspect(QAccessibleInterface *node, const QString &sample,
             QSet<QAccessibleInterface *> &visited, Observation &result)
{
    if (!node || visited.contains(node))
        return;
    if (visited.size() >= 256) {
        result.complete = false;
        return;
    }
    visited.insert(node);
    if (!node->isValid()) {
        result.complete = false;
        return;
    }
    ++result.nodes;
    if (auto *text = node->textInterface()) {
        ++result.textInterfaces;
        const int count = text->characterCount();
        if (count < 0 || count > 4096) {
            result.complete = false;
            return;
        }
        result.sampleInText |= text->text(0, count).contains(sample);
    }
    if (node->editableTextInterface())
        ++result.editableTextInterfaces;
    result.sampleInValue |= node->text(QAccessible::Value).contains(sample);
    const int children = node->childCount();
    if (children < 0 || children > 256) {
        result.complete = false;
        return;
    }
    for (int i = 0; i < children; ++i) {
        QAccessibleInterface *child = node->child(i);
        if (!child) {
            result.complete = false;
            return;
        }
        inspect(child, sample, visited, result);
    }
}

Observation observe(QWidget &widget, const QString &sample)
{
    Observation result;
    QSet<QAccessibleInterface *> visited;
    QAccessibleInterface *root = QAccessible::queryAccessibleInterface(&widget);
    if (!root) {
        result.complete = false;
        return result;
    }
    inspect(root, sample, visited, result);
    return result;
}

void report(const char *name, const Observation &result)
{
    qInfo().nospace() << name << ": nodes=" << result.nodes
                      << " textInterfaces=" << result.textInterfaces
                      << " editableTextInterfaces=" << result.editableTextInterfaces
                      << " sampleInText=" << result.sampleInText
                      << " sampleInValue=" << result.sampleInValue
                      << " complete=" << result.complete;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QAccessible::setActive(true);
    const QString sample = QStringLiteral("读写🙂 sample");

    QPlainTextEdit baseline;
    baseline.setPlainText(sample);
    const Observation baselineResult = observe(baseline, sample);
    report("QPlainTextEdit", baselineResult);
    if (!baselineResult.complete || !baselineResult.sampleInText
        || baselineResult.textInterfaces == 0 || baselineResult.editableTextInterfaces == 0) {
        qCritical("The Qt accessibility baseline did not expose its editable text");
        return 1;
    }

    ScintillaEditBase candidate;
    candidate.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    const QByteArray bytes = sample.toUtf8();
    candidate.send(SCI_ADDTEXT, bytes.size(), reinterpret_cast<sptr_t>(bytes.constData()));
    const Observation candidateResult = observe(candidate, sample);
    report("ScintillaEditBase", candidateResult);
    // A negative observation for this pinned candidate, not a product policy.
    // If an upgrade exposes the sample, the comparison must be re-evaluated.
    if (!candidateResult.complete || candidateResult.sampleInText || candidateResult.sampleInValue) {
        qCritical("The candidate accessibility observation changed or was incomplete");
        return 1;
    }
    return 0;
}
