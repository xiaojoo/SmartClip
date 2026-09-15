# -*- coding: utf-8 -*-
"""文档 / 图片 -> Markdown：**省资源那条路**（IBM Granite-Docling-258M）。

由 SmartClip 调起，别自己双击跑。约定见 doc_runner_common.py 的开头。

这是三个引擎里最小巧的一个：258M（权重约 515MB），按 DocTags 的结构化标记
整页转写，再交给 Docling 拼成 Markdown。表格结构（FinTabNet TEDS 0.97）和公式
（F1 0.968）在同尺寸里很能打，**但语言以英文为主**（中文/日文/阿拉伯文官方标为
实验性、未验证），手写体也不是它的强项 —— 中文材料还是优先 PaddleOCR-VL。

--------------------------------------------------------------------------
装依赖

    python -m pip install docling
    python -m pip install "transformers>=4.46" torch --index-url https://download.pytorch.org/whl/cu124

第一次跑会下模型（约 515MB）。

--------------------------------------------------------------------------
档位（第三个参数，可选）

    fast       CPU 也能跑（慢）：一页一页来
    balanced   默认：小批量
    best       多页批处理 + 更细的版面

也可以给一份 **params.json 路径**：

    {"device": "cuda", "batch_size": 4, "scale": 2.0, "layout_model": "docling-project/granite-docling-2stage-258m"}

`device` 给 "cuda" / "cpu"；想指定别的卡就写 "cuda:1"。
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from doc_runner_common import run_main  # noqa: E402

TIERS = {
    "fast": {"batch_size": 1, "scale": 2.0},
    "balanced": {"batch_size": 2, "scale": 2.0},
    "best": {"batch_size": 4, "scale": 2.5},
}

DEFAULT_TIER = "balanced"


def _engine_version():
    try:
        import importlib.metadata as meta

        return "Granite-Docling-258M + docling %s" % meta.version("docling")
    except Exception:
        return "Granite-Docling-258M"


def _resolve_device(device):
    device = (device or "").strip().lower()
    if device:
        return device
    try:
        import torch

        return "cuda" if torch.cuda.is_available() else "cpu"
    except Exception:
        return "cpu"


def convert(input_path, tier):
    """
    走 Docling 的 VLM 流水线。

    为什么用 Docling 而不是自己调模型：Granite-Docling 产出的是 DocTags（带
    <loc_…> 坐标的结构标记），要转成 Markdown 得靠 docling-core 那套解析 ——
    自己写等于把 Docling 重写一遍。官方也是这个用法（docling --pipeline vlm）。
    """
    from docling.datamodel.base_models import InputFormat
    from docling.datamodel.pipeline_options import VlmPipelineOptions
    from docling.document_converter import DocumentConverter, PdfFormatOption
    from docling.pipeline.vlm_pipeline import VlmPipeline

    params = dict(TIERS[DEFAULT_TIER])
    if tier:
        if tier.lower() in TIERS:
            params = dict(TIERS[tier.lower()])
        else:
            with open(tier, "r", encoding="utf-8") as handle:
                params.update(json.load(handle))

    device = _resolve_device(params.pop("device", ""))
    batch_size = int(params.pop("batch_size", 2))
    scale = float(params.pop("scale", 2.0))
    model = params.pop("layout_model", "ibm-granite/granite-docling-258M")

    try:
        from docling.datamodel.pipeline_options import VlmConvertOptions
        from docling.datamodel.vlm_engine_options import TransformersVlmEngineOptions

        engine_options = TransformersVlmEngineOptions(
            model=model,
            scale=scale,
            batch_size=batch_size,
            # 有 GPU 就放 GPU 上，bfloat16 省显存
            **({"device": device} if device else {}),
        )
        pipeline_options = VlmPipelineOptions(
            vlm_options=VlmConvertOptions(engine_options=engine_options),
        )
    except Exception:
        """
        参数名对不上（docling 的小版本之间改过好几次）。

        退回最保守的一套：让 docling 用它自己的默认模型和默认设备。宁可慢、
        宁可精度不是最好的，也不要"装了个别的版本就完全不能用"。
        """
        pipeline_options = VlmPipelineOptions()

    converter = DocumentConverter(
        format_options={
            InputFormat.PDF: PdfFormatOption(
                pipeline_cls=VlmPipeline, pipeline_options=pipeline_options),
        }
    )

    result = converter.convert(input_path)
    document = result.document

    markdown = document.export_to_markdown()

    # Docling 把页面里裁出来的图挂在 document.pictures 上；有就一起给出去
    images = {}
    for index, picture in enumerate(getattr(document, "pictures", []) or []):
        try:
            image = picture.get_image(document)
            if image is not None:
                images["page-%02d-fig-%02d.png" % (index + 1, index + 1)] = image
        except Exception:
            continue

    pages = None
    try:
        pages = len(document.pages)
    except Exception:
        pages = None

    return {
        "markdown": markdown,
        "images": images,
        "pages": pages,
        "title": os.path.splitext(os.path.basename(input_path))[0],
        "engine": "%s (%s, %s)" % (_engine_version(), tier or DEFAULT_TIER, device),
    }


if __name__ == "__main__":
    sys.exit(run_main("Granite-Docling", convert))
