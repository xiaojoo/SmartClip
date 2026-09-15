# -*- coding: utf-8 -*-
"""文档识别 runner 的**共用契约** —— SmartClip 调的那几个脚本都 import 它。

三个 runner（rapid / paddleocr_vl / granite）长得不一样，但和 SmartClip 之间的
约定只有这一份。放在单独一个文件里是因为约定一改就是三个一起改，抄三遍迟早
有一份忘了改 —— 那个 runner 会在某天突然不工作。

--------------------------------------------------------------------------
约定（SmartClip 就是这么调的）

    <整条命令…>  <输入文件路径>  <结果.json 路径>

两个路径**在最后**（SmartClip 接在整条命令末尾），脚本自己的参数排在前面 ——
和 ppocr_runner.py 同一个理由：`python 脚本.py …` 里脚本路径必须是 python 的
第一个参数，文件只能往后排。所以这里读的是 `argv[-2]` / `argv[-1]`，前面那些
才是"要哪一档"。

--------------------------------------------------------------------------
写出去的结果（JSON 对象）

    {
      "markdown": "# 标题\\n\\n正文…",
      "images":   {"fig1.png": "<base64 的 PNG>"},
      "engine":   "rapid-doc 0.9.10",
      "pages":    12,
      "title":    "可选，笔记标题",
      "error":    "可选，出错时的人话（有它就别写 markdown）"
    }

`images` 的键就是 markdown 里引用的**文件名**；SmartClip 会把它们落到笔记目录的
assets/ 下，并把正文里的引用改写成 `assets/<名字>`。所以 runner 只管把图给出来、
名字取好，**不用管文件最终存在哪**。

--------------------------------------------------------------------------
出错怎么办

**一定要写结果文件**，哪怕是把错误写进 `error` 字段。SmartClip 那边"没写出
结果"和"写出来了但内容是错误"是两条不同的提示：前者只能把子进程的输出截一段
给你看（多半是几百行进度条），后者能直接把 `error` 那句话摆到界面上。
"""
from __future__ import annotations

import base64
import io
import json
import os
import sys
import traceback

# 输出里图片的默认目录名（markdown 里会写成 images/<名字>，SmartClip 再改写）
DEFAULT_IMAGE_DIR = "images"


def read_args(argv):
    """从命令行里取出 (输入路径, 结果路径, 档位词)。

    档位词在中间是可选的（`script.py best <输入> <结果.json>`），所以规则是：
    最后两个一定是路径，**倒数第三个（如果有）就是档位词**。
    """
    args = argv[1:]
    if len(args) < 2:
        sys.stderr.write(
            "用法：%s [档位词] <输入文件> <结果.json>\n" % os.path.basename(argv[0]))
        raise SystemExit(2)
    input_path, out_path = args[-2], args[-1]
    tier = args[-3] if len(args) > 2 else ""
    if tier and os.path.sep in tier:
        # 倒数第三个看着像个路径 -> 那它其实是输入路径，没给档位词
        tier = ""
    return input_path, out_path, tier


def write_result(out_path, **fields):
    """把结果写进那个文件。**必须**成功 —— 这正是 SmartClip 唯一的读数口。"""
    payload = {k: v for k, v in fields.items() if v is not None}
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False)


def image_to_base64(data):
    """bytes -> 纯 base64 串（不带 data URI 头；带 SmartClip 也认，但不带更干净）"""
    return base64.b64encode(data).decode("ascii")


def pil_to_png_bytes(image):
    """PIL 图 -> PNG bytes（有的库里给的是 PIL 对象，需要自己编码一下）"""
    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def normalise_images(images):
    """把引擎给的图片表整成 {"文件名": "<base64>"}。

    认得三种写法（各家引擎给的不一样）：
      * {"名字": b"..."}          —— 二进制，最常见
      * {"名字": "已经 base64"}   —— 少数引擎直接给串
      * [{"name":…, "data":…}]    —— 数组形式
    PIL 对象自动编码成 PNG。
    """
    out = {}
    if not images:
        return out

    items = images.items() if isinstance(images, dict) else [
        (entry.get("name"), entry.get("data"))
        for entry in images
        if isinstance(entry, dict)
    ]

    for name, data in items:
        if not name or data is None:
            continue
        name = os.path.basename(str(name))
        if not name:
            continue
        if isinstance(data, str):
            payload = data
        elif isinstance(data, (bytes, bytearray)):
            payload = image_to_base64(bytes(data))
        else:
            # PIL 图之类：编码成 PNG
            try:
                payload = image_to_base64(pil_to_png_bytes(data))
            except Exception:
                continue
        out[name] = payload
    return out


def run_main(engine_name, convert, argv=None):
    """runner 的统一外壳：读参数 -> 调 convert -> 写结果 -> 兜住所有异常。

    convert(input_path, tier) 要返回一个 dict，键就是上面那套
    （markdown / images / pages / title）；别的键忽略。

    异常一律变成结果文件里的 `error` —— 见模块开头那段"出错怎么办"。
    """
    argv = sys.argv if argv is None else argv
    out_path = None
    try:
        input_path, out_path, tier = read_args(argv)
    except SystemExit:
        raise
    except Exception as exc:  # pragma: no cover - 参数解析不该走到这里
        sys.stderr.write("参数看不懂：%s\n" % exc)
        return 2

    try:
        if not os.path.isfile(input_path):
            raise FileNotFoundError("找不到这个文件：%s" % input_path)

        result = convert(input_path, tier) or {}
        images = normalise_images(result.get("images"))
        write_result(
            out_path,
            markdown=result.get("markdown") or "",
            images=images or None,
            engine=result.get("engine") or engine_name,
            pages=result.get("pages"),
            title=result.get("title"),
        )
        return 0

    except SystemExit:
        raise
    except BaseException as exc:
        """
        所有异常都收在这儿，写进结果文件的 error 字段。

        为什么要 traceback：SmartClip 只显示 error 那一句，而真正的原因常常在
        下面几层（比如"缺某个依赖"实际是 ImportError 包着一条 DLL 找不到）。
        把最后几行拼进 error 里，用户在界面上就能看出个大概，不用去翻控制台。
        """
        detail = traceback.format_exc(limit=6).strip().splitlines()
        tail = " | ".join(line.strip() for line in detail[-3:]) if detail else ""
        message = "%s：%s" % (engine_name, exc)
        if tail and tail not in message:
            message = "%s（%s）" % (message, tail)
        try:
            write_result(out_path, error=message)
        except Exception:
            sys.stderr.write(message + "\n")
            return 1
        return 0
