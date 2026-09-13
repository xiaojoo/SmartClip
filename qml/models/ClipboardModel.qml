pragma ComponentBehavior: Bound

import QtQuick
import SmartClip.Globals 1.0

/*
  ClipboardModel.qml
  左树的原始数据（来自 C++ 的 Store 单例）。

  数据已经不是"一堆剪贴板条目"，而是**嵌套的两级树**：
  日期文件夹（2026-09-13）-> 里面的 md 文件（073100.md）；
  导入的外部文件夹是另一棵，子目录会一层层挂下去。
  真正拍平成行、按展开状态取舍，是 js/FolderManager.js 的事。

  注意：这里只保存数据，不要声明 spacing / model / delegate 之类的视图属性，
  否则会与基类的 FINAL 成员冲突（Qt 6.11 会直接报错：无法重写 FINAL 属性）。
  间距等 UI 属性请放在 delegate / 容器里（例如 TreeDelegate、ColumnLayout）。
*/
QtObject {
    id: root

    property var nodes: []
    signal changed()

    function reload(query, newestFirst) {
        nodes = Store.tree(query || "", newestFirst !== false)
        changed()
    }
}
