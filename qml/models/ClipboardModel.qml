pragma ComponentBehavior: Bound

import QtQuick
import SmartClip.Globals 1.0

/*
  ClipboardModel.qml
  剪贴板条目数据模型（数据来自 C++ 的 Store 单例）。

  注意：这里只保存数据，不要声明 spacing / model / delegate 之类的视图属性，
  否则会与基类的 FINAL 成员冲突（Qt 6.11 会直接报错：无法重写 FINAL 属性）。
  间距等 UI 属性请放在 delegate / 容器里（例如 TreeDelegate、ColumnLayout）。
*/
QtObject {
    id: root

    property var entries: []
    signal changed()

    function reload(query) {
        entries = Store.items(query || "")
        changed()
    }
}
