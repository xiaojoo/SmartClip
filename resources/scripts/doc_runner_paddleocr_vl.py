# -*- coding: utf-8 -*-
"""文档 / 图片 -> Markdown：**精度优先那条路**（PaddleOCR-VL 1.6）。

由 SmartClip 调起，别自己双击跑。约定见 doc_runner_common.py 的开头。

这条路比默认的 RapidDoc 重得多，换来的是**版面理解**：它是个 VLM（0.9B），
按"看清了再写"的方式整页转写，表格 / 公式 / 多栏 / 竖排 / 手写都比纯 OCR
流水线强，而且支持 109 种语言。

--------------------------------------------------------------------------
装依赖（大概 3~5GB，先想好放哪）

    # GPU（有 NVIDIA 卡就走这条，快很多）
    python -m pip install paddlepaddle-gpu==3.2.1 -i https://www.paddlepaddle.org.cn/packages/stable/cu126/
    # CPU
    python -m pip install paddlepaddle==3.2.1 -i https://www.paddlepaddle.org.cn/packages/stable/cpu/

    python -m pip install -U "paddleocr[doc-parser]>=3.6.0"

第一次跑会自动下模型（PaddleOCR-VL 0.9B 约 1.8GB + 版面模型）。

**注意**：vLLM / SGLang / FastDeploy 这些加速后端在 Windows 上跑不了（要
Docker）。这里走的是官方支持的 **PaddlePaddle 原生后端**，Windows 上没问题。

--------------------------------------------------------------------------
档位（第三个参数，可选）

    fast       CPU 也能忍：一页一页来，关掉图表 / 印章识别
    balanced   默认
    best       图表 / 印章 / 图片块 OCR 全开（版面元素认得最全）

也可以给一份 **params.json 路径** —— 键名就是 PaddleOCRVL 的构造参数
（`device` / `engine` / `precision` / `use_chart_recognition` …）：

    {"device": "gpu:0", "use_chart_recognition": true, "use_seal_recognition": true}

`device` 给 "gpu" 会用第 0 号卡（会自动展开成 "gpu:0"）；"cpu" 就是纯 CPU。

--------------------------------------------------------------------------
和 1.6 这版对参数名（踩过）

这版 PaddleOCRVL 的构造参数里**没有** `use_formula_recognition` /
`use_table_recognition` —— 表格和公式是 VLM 自己吐出来的，没有单独的开关。
它有的是：

    use_doc_orientation_classify / use_doc_unwarping / use_layout_detection /
    use_chart_recognition / use_seal_recognition / use_ocr_for_image_block

传错名字它抛的是 **ValueError**（不是 TypeError），所以下面那个兜底两种都要接。
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from doc_runner_common import run_main  # noqa: E402

TIERS = {
    # 最省：关掉图表和印章（这两样各要过一次模型）
    "fast": {"use_chart_recognition": False, "use_seal_recognition": False},
    "balanced": {},
    # 版面元素认全：图表、印章、图片块里的字都过一遍
    "best": {"use_chart_recognition": True, "use_seal_recognition": True,
             "use_ocr_for_image_block": True},
}

DEFAULT_TIER = "balanced"


def _engine_version():
    try:
        import importlib.metadata as meta

        return "PaddleOCR-VL %s" % meta.version("paddleocr")
    except Exception:
        return "PaddleOCR-VL"


def _resolve_device(device):
    """把 "gpu" / "gpu:1" / "cpu" 整成 paddle 认的设备名。

    写成 "gpu" 是给人填的（设置面板里那行小字），paddle 要的是 "gpu:0"。
    """
    device = (device or "").strip().lower()
    if not device:
        return "gpu:0" if _cuda_available() else "cpu"
    if device == "gpu":
        return "gpu:0"
    return device


def _cuda_available():
    try:
        import paddle

        return bool(paddle.device.cuda.device_count())
    except Exception:
        return False


def _build_pipeline(extra):
    """extra 是档位词 / params.json 路径 / 空。"""
    from paddleocr import PaddleOCRVL

    params = dict(TIERS[DEFAULT_TIER])
    if extra:
        if extra.lower() in TIERS:
            params = dict(TIERS[extra.lower()])
        else:
            with open(extra, "r", encoding="utf-8") as handle:
                params.update(json.load(handle))

    device = _resolve_device(params.pop("device", ""))
    try:
        pipeline = PaddleOCRVL(device=device, **params)
    except (TypeError, ValueError):
        """
        参数名对不上（这版之间改过，见文件头）时退回最保守的一套：只给设备。

        宁可慢、宁可少认几样版面元素，也不要"装了个别的版本就完全不能用"。
        """
        pipeline = PaddleOCRVL(device=device)
    return pipeline, device


def _markdown_of(item):
    """
    从一页的结果里把 Markdown 抠出来。

    这版（paddleocr 3.7 / PaddleOCR-VL 1.6）的结构是：

        item            PaddleOCRVLResult —— 是 dict 的子类，但 markdown 不在它的键里
        item.markdown   **property**，返回 {"markdown_texts": "…", "markdown_images": {…},
                                            "page_index": 0, "input_path": "…"}

    踩过的坑：`markdown` 不是字符串，而是一个 dict —— 一开始按 `isinstance(value, str)`
    判，永远匹配不上，结果"跑通了但 markdown 是空的"（页数 1、正文全空）。
    所以这里先按 dict 取 markdown_texts，再退回字符串那条老路。
    """
    value = getattr(item, "markdown", None)

    if isinstance(value, dict):
        for name in ("markdown_texts", "markdown_text"):
            text = value.get(name)
            if isinstance(text, str) and text.strip():
                return text

    if isinstance(value, str) and value.strip():
        return value

    for name in ("markdown_texts", "markdown_text", "md"):
        text = getattr(item, name, None)
        if isinstance(text, str) and text.strip():
            return text

    # 有的版本结果本身就是 dict
    if isinstance(item, dict):
        for name in ("markdown_texts", "markdown", "md"):
            text = item.get(name)
            if isinstance(text, str) and text.strip():
                return text
    return ""


def _images_of(item):
    """
    图片：这版放在 `item.markdown["markdown_images"]` 里（键是文件名，值是图）。

    PaddleOCR-VL 默认还会把图**内联成 data URI 写进 markdown**，那种情况下这里
    是空的 —— 空就空着，正文里的图照样能显示（DocConvert 那边两种都认）。
    """
    value = getattr(item, "markdown", None)
    if isinstance(value, dict):
        images = value.get("markdown_images")
        if isinstance(images, dict) and images:
            return images

    for name in ("images", "image_map", "imgs"):
        got = getattr(item, name, None)
        if got:
            return got
    return {}


def convert(input_path, tier):
    pipeline, device = _build_pipeline(tier)

    # predict 返回的是一个可迭代的结果集，一页一条
    results = list(pipeline.predict(input_path))

    chunks = []
    images = {}
    for item in results:
        piece = _markdown_of(item)
        if piece:
            chunks.append(piece)
        got = _images_of(item)
        if isinstance(got, dict):
            images.update(got)

    return {
        "markdown": "\n\n".join(chunks),
        "images": images,
        "pages": len(results),
        "title": os.path.splitext(os.path.basename(input_path))[0],
        "engine": "%s (%s, %s)" % (_engine_version(), tier or DEFAULT_TIER, device),
    }


if __name__ == "__main__":
    sys.exit(run_main("PaddleOCR-VL", convert))
