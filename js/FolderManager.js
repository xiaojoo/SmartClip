.pragma library

/*
 * 把 Store 给的**嵌套树**拍成左树要画的平铺行。
 *
 * 入参 nodes 见 ClipboardStore::tree()：
 *   文件夹 { key, label, kind("date"/"imported"/"folder"), path, depth, files, entries, children }
 *   文件   { key(path), label, kind: "file", path, depth, entries, size, imported }
 *
 * expanded 是"哪些文件夹展开了"的映射（按 key，见 Main.qml 的 expanded）。
 * 折叠的文件夹只出行本身，它的子节点整段跳过 —— 这就是虚拟化列表里
 * 决定"总行数"的地方。
 */

function buildTree(nodes, expanded) {
    var rows = []
    walk(nodes || [], rows, expanded)
    return rows
}

function walk(list, rows, expanded) {
    for (var i = 0; i < list.length; ++i) {
        var node = list[i]
        if (!node)
            continue

        if (node.kind === "file") {
            rows.push({
                kind: "file",
                key: node.key,
                label: node.label,
                depth: node.depth,
                path: node.path,
                entries: node.entries,
                size: node.size,
                imported: !!node.imported,
                dateKey: node.dateKey
            })
            continue
        }

        var open = !!expanded[node.key]
        rows.push({
            kind: "folder",
            key: node.key,
            label: node.label,
            depth: node.depth,
            path: node.path,
            /* date（我们自己记的日期目录）/ imported（导入的文件夹）/ folder（导入目录里的子目录） */
            folderKind: node.kind,
            files: node.files,
            entries: node.entries,
            expanded: open
        })

        if (open)
            walk(node.children || [], rows, expanded)
    }
}

/* 树里所有文件夹的 key（"全部展开 / 全部折叠"用） */
function allFolderKeys(nodes) {
    var out = []
    collectFolderKeys(nodes || [], out)
    return out
}

function collectFolderKeys(list, out) {
    for (var i = 0; i < list.length; ++i) {
        var node = list[i]
        if (!node || node.kind === "file")
            continue
        out.push(node.key)
        collectFolderKeys(node.children || [], out)
    }
}

/* 第一层里第一个"日期"文件夹的 key（启动时默认展开今天那一组） */
function firstDateKey(nodes) {
    for (var i = 0; i < (nodes || []).length; ++i)
        if (nodes[i] && nodes[i].kind === "date")
            return nodes[i].key
    return ""
}
