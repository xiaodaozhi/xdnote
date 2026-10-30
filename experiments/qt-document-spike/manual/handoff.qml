import QtQuick
import QtQuick.Window

QtObject {
    component TestWindow: Window {
        id: win
        required property int windowIndex
        visible: true
        width: 620
        height: 330
        title: windowIndex === 0 ? "文档接管试验 — 独立窗口" : "文档接管试验 — 工作区窗口"
        color: "#f4f5f6"

        Text {
            id: description
            x: 16; y: 14
            width: parent.width - 32
            text: (harness.activeWindow === win.windowIndex ? "当前编辑窗口" : "非活动窗口（只读）")
                  + "；使用 F8 或按钮向另一窗口接管"
            font.pixelSize: 15
        }
        Rectangle {
            id: editorFrame
            x: 16; y: 46
            width: parent.width - 32; height: 170
            color: "white"; border.color: "#aab0b6"; clip: true
            TextEdit {
                id: editor
                objectName: win.windowIndex === 0 ? "standaloneEditor" : "workspaceEditor"
                anchors.fill: parent; anchors.margins: 10
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                readOnly: harness.activeWindow !== win.windowIndex
                selectByMouse: true
                persistentSelection: true
                font.pixelSize: 18
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_F8 && !readOnly) {
                        harness.requestHandoff()
                        event.accepted = true
                    }
                }
            }
        }
        Rectangle {
            id: button
            x: 16; y: 228; width: 230; height: 36
            radius: 4; color: "#dce1e5"
            Text { anchors.centerIn: parent; text: "接管到另一窗口（F8）"; font.pixelSize: 14 }
            MouseArea { anchors.fill: parent; onClicked: harness.requestHandoff() }
        }
        Text {
            x: 258; y: 237
            text: "本窗口输入法组合中：" + (editor.inputMethodComposing ? "是" : "否")
            font.pixelSize: 13
        }
        Text {
            x: 16; y: 280; width: parent.width - 32
            text: harness.status
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
    }

    property TestWindow standaloneWindow: TestWindow { windowIndex: 0; x: 60; y: 60 }
    property TestWindow workspaceWindow: TestWindow { windowIndex: 1; x: 720; y: 60 }
}
