# -*- coding: utf-8 -*-
"""文档 / 图片 -> Markdown：**默认那条路**（RapidDoc，纯 ONNX，CPU 就够）。

由 SmartClip 调起，别自己双击跑。约定见 doc_runner_common.py 的开头。

装依赖：

    pip install rapid-doc

RapidDoc 是 RapidAI 那套 ONNX 文档流水线：版面分析 + OCR（内置 RapidOCR）+
表格结构 + 公式识别，全在一个包和它自带的模型里（约 800MB，装完就能离线跑，
**不需要** paddlepaddle / torch）。它和"图上选字"用的那个 RapidOCR 是同一家，
区别是这里多了版面、表格、公式和阅读顺序。

--------------------------------------------------------------------------
档位（第三个参数，可选）

    fast       只做版面和文字（关掉表格与公式识别）—— 最快
    balanced   默认：版面 + 文字 + 表格 + 公式
    best       在 balanced 基础上一次处理更多页、更激进的排版还原

也可以给一份 **params.json 路径**（和 ppocr_runner.py 一个做法），想调更细的
参数时用它：

    {"formula_enable": true,
     "layout_config": {"engine_type": "onnxruntime"},
     "ocr_config": {"Det.model_type": "medium"}}

--------------------------------------------------------------------------
GPU

默认走 CPU（稳、离线、不用配）。这台机器有 NVIDIA 卡、且装了
`onnxruntime-gpu` 的话，把 params.json 里加上：

    {"layout_config": {"engine_cfg": {"onnxruntime": {"use_cuda": true}}}}

RapidDoc 的每一段（版面 / 表格 / 公式）各有一份 engine_cfg，要开就三份都开。
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from doc_runner_common import run_main  # noqa: E402

# 档位 -> RapidDoc 的开关。键名和 RapidDoc 的构造参数一致。
TIERS = {
    # 只要版面和文字：表格 / 公式这两段（最慢的两段）整个不加载
    "fast": {"formula_enable": False, "table_enable": False, "pdf_pages_batch": 16},
    "balanced": {"formula_enable": True, "table_enable": True, "pdf_pages_batch": 32},
    "best": {"formula_enable": True, "table_enable": True, "pdf_pages_batch": 64},
}

DEFAULT_TIER = "balanced"


def _engine_version():
    try:
        from rapid_doc.version import __version__

        return "rapid-doc %s" % __version__
    except Exception:
        try:
            import importlib.metadata as meta

            return "rapid-doc %s" % meta.version("rapid-doc")
        except Exception:
            return "rapid-doc"


def _build_engine(extra):
    """extra 是档位词 / params.json 路径 / 空。"""
    from rapid_doc import RapidDoc

    params = dict(TIERS.get(DEFAULT_TIER or "balanced"))

    if extra:
        if extra.lower() in TIERS:
            params = dict(TIERS[extra.lower()])
        else:
            with open(extra, "r", encoding="utf-8") as handle:
                params.update(json.load(handle))

    # 图表标题之类的语言，ch 覆盖中英混排（RapidDoc 的默认值，写出来免得被误改）
    params.setdefault("lang", "ch")
    return RapidDoc(**params)


def convert(input_path, tier):
    engine = _build_engine(tier)
    output = engine(input_path)

    # 有的版本单文档也回一个列表（输入被当成了 batch），两种都接住
    if isinstance(output, list):
        output = output[0]

    images = getattr(output, "images", None) or {}
    pages = None
    middle = getattr(output, "middle_json", None)
    if isinstance(middle, dict):
        try:
            pages = len(middle.get("pdf_info") or [])
        except Exception:
            pages = None

    return {
        "markdown": getattr(output, "markdown", "") or "",
        "images": images,
        "pages": pages,
        "title": os.path.splitext(os.path.basename(input_path))[0],
        "engine": "%s (%s)" % (_engine_version(), tier or DEFAULT_TIER),
    }


if __name__ == "__main__":
    sys.exit(run_main("RapidDoc", convert))
