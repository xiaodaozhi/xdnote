#include "MacUpdateProvider.h"
#include "UpdateController.h"
#include "GuiRuntime.h"
#include <QApplication>
#include <QTest>
class MacUpdateTests final : public QObject {
    Q_OBJECT
private slots:
    void nativeLifecycleHoldsCommitAndWaitsForExactHelperExit() {
        xdnote::UpdateController owner;
        QVERIFY(macUpdateLifecycleProbe(&owner));
    }
};
int main(int argc, char **argv) {
    xdnote::initializeGuiRuntime(); QApplication app(argc, argv);
    MacUpdateTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "mac_update_tests.moc"
