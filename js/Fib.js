.pragma library

// 带缓存的斐波那契数列 (memoization)
// 用法:
//   import "../js/Fib.js" as Fib
//   Fib.fib(30)          // 1028320, 重复调用直接命中缓存
//   Fib.fibIterative(90) // 大数值时避免递归深度问题
//   Fib.clearCache()     // 清空缓存

// 基础缓存: 直接放入已知的 fib(0) 与 fib(1)
var _cache = { "0": 0, "1": 1 }

// 统计命中/未命中, 便于调试
var _hits = 0
var _misses = 0

function _normalize(n) {
    n = Math.floor(Number(n))
    if (!isFinite(n) || isNaN(n))
        return -1
    return n
}

// 递归 + 记忆化实现
function fib(n) {
    n = _normalize(n)
    if (n < 0)
        return 0

    if (_cache[n] !== undefined) {
        _hits += 1
        return _cache[n]
    }

    _misses += 1
    var value = fib(n - 1) + fib(n - 2)
    _cache[n] = value
    return value
}

// 迭代实现, 同样走缓存, 适合 n 较大 (避免递归过深)
function fibIterative(n) {
    n = _normalize(n)
    if (n < 0)
        return 0

    if (_cache[n] !== undefined) {
        _hits += 1
        return _cache[n]
    }

    var known = 1
    while (_cache[known + 1] !== undefined)
        known += 1

    var a = _cache[known] !== undefined ? _cache[known] : 1
    var b = _cache[known - 1] !== undefined ? _cache[known - 1] : 0

    for (var i = known + 1; i <= n; ++i) {
        var next = a + b
        _cache[i] = next
        b = a
        a = next
    }

    _misses += 1
    return _cache[n]
}

// 返回 0..n 的斐波那契序列
function fibSequence(n) {
    n = _normalize(n)
    var result = []
    if (n < 0)
        return result
    for (var i = 0; i <= n; ++i)
        result.push(fib(i))
    return result
}

// 清空缓存, 恢复到初始状态
function clearCache() {
    _cache = { "0": 0, "1": 1 }
    _hits = 0
    _misses = 0
}

// 当前缓存了多少个数
function cacheSize() {
    return Object.keys(_cache).length
}

// 缓存是否已包含 fib(n)
function isCached(n) {
    n = _normalize(n)
    return n >= 0 && _cache[n] !== undefined
}

// 调试统计
function stats() {
    return { "hits": _hits, "misses": _misses, "size": cacheSize() }
}
