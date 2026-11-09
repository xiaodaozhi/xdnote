import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Xdnote.Internal 1.0

QuickAccessibleRoot {
    accessibilityHost: chromeHost
    Accessible.role: Accessible.Grouping
    Accessible.name: "文档操作"
    Rectangle {
        anchors.fill: parent
        color: visual.themes.light.surface
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 8
            Text {
                text: "xdnote"
                color: visual.themes.light.text
                font.pixelSize: visual.typography.ui.size
                font.weight: Font.DemiBold
            }
            Text {
                text: admission.feedback
                color: visual.themes.light.textSecondary
                font.pixelSize: visual.typography.secondary.size
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            SharedButton { text: "撤销"; onClicked: admission.undo() }
            SharedButton { text: "重做"; onClicked: admission.redo() }
            SharedButton { objectName: "readOnlyButton"; text: admission.readOnly ? "恢复编辑" : "设为只读"; onClicked: admission.toggleReadOnly() }
        }
    }
}
