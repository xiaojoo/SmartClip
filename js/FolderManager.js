.pragma library

// ---- builds the folder/date tree rows for the left panel ----

function periodCount(entries, key, periodFor) {
    var n = 0
    for (var i = 0; i < entries.length; i++)
        if (periodFor(entries[i].createdAt) === key) n++
    return n
}

function buildTree(entries, folders, expanded, periodFor) {
    var rows = []
    for (var i = 0; i < folders.length; i++) {
        var f = folders[i]
        rows.push({ kind: "folder", key: f.key, label: f.label,
                    count: periodCount(entries, f.key, periodFor), expanded: !!expanded[f.key] })
        if (expanded[f.key]) {
            for (var j = 0; j < entries.length; j++)
                if (periodFor(entries[j].createdAt) === f.key)
                    rows.push({ kind: "item", item: entries[j] })
        }
    }
    return rows
}
