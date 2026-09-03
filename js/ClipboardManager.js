.pragma library

// ---- thin wrapper around the clipboardStore context property ----

function items(query) {
    return clipboardStore.items(query || "")
}

function copyItem(id) {
    clipboardStore.copyItem(id)
}
