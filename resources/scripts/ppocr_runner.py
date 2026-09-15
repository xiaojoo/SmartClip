# -*- coding: utf-8 -*-
"""贴图「图上选字」用的 PP-OCR 小脚本 —— 由 SmartClip 调起，别自己双击跑。

它只干一件事：把 RapidOCR / PaddleOCR 和 SmartClip 之间的**约定**翻译一下。

约定（SmartClip 就是这么调的）：

    python ppocr_runner.py  [档位 或 params.json]  <图片路径>  <结果.json 路径>

两个路径**在最后**（SmartClip 接在整条命令末尾），脚本自己的参数排在前面 ——
所以这里读的是 `sys.argv[-2]` 和 `sys.argv[-1]`，前面那些才是"要哪一档"。
（顺序定成这样是被 python 逼的：`python 脚本.py …` 里脚本路径必须是第一个参数，
图片只能往后排。）

SmartClip 先把贴图的底图存成 PNG，再调起这条命令；脚本把认出来的每一行写进
第二个参数那个文件：

    [{"text": "第一行", "box": [x, y, w, h]}, ...]

box 是**图片像素**坐标（x/y 是左上角，w/h 是宽高）。归一化由 SmartClip 自己做
（它知道图片多大），所以脚本不用管缩放、不用读图尺寸。

装依赖（只这两个，**不用装 paddlepaddle**）：

    pip install rapidocr onnxruntime

RapidOCR 3.9 起内置 PP-OCRv6（默认是 **small** 档），第一次跑会自动下模型。
第三个参数可以给：

  * 一个**档位词** —— `tiny` / `small` / `medium`：直接用 PP-OCRv6 对应那一档
    （medium 最准也最慢：CPU 上检测一次一秒上下；模型第一次跑会下，约 135MB）。
    例：`… python ppocr_runner.py <图> <结果.json> medium`
  * 或者一份 **params.json 的路径** —— 想指到自己下的 ONNX 文件、或者调别的参数
    （键名就是 RapidOCR 的参数名）时用它：

    {"Det.model_path": "D:/models/PP-OCRv6_medium_det.onnx",
     "Rec.model_path": "D:/models/PP-OCRv6_medium_rec.onnx",
     "Rec.rec_keys_path": "D:/models/ppocrv6_dict.txt"}
"""
from __future__ import annotations

import json
import sys

# PP-OCRv6 的三档（RapidOCR 的 Det/Rec.model_type 就认这三个词）
TIERS = ("tiny", "small", "medium")


def _poly_to_box(poly):
    """四边形（4 个点）-> 外接矩形 [x, y, w, h]（像素）"""
    xs = [float(p[0]) for p in poly]
    ys = [float(p[1]) for p in poly]
    x0, y0 = min(xs), min(ys)
    return [x0, y0, max(xs) - x0, max(ys) - y0]


def _build_engine(extra):
    """extra 是第三个参数：档位词 / params.json 路径 / 没给"""
    # 到这一步才 import：没装 rapidocr 时报错信息最短、最好看懂
    from rapidocr import RapidOCR

    if not extra:
        return RapidOCR()
    if extra.lower() in TIERS:
        # 注意：RapidOCR 的参数表要的是**枚举**，写字符串它直接抛
        # "The value of Det.model_type must be Enum Type."
        from rapidocr import ModelType

        tier = getattr(ModelType, extra.upper(), None)
        if tier is None:
            raise SystemExit(f"这版 rapidocr 没有「{extra}」这一档（认得的：{'/'.join(TIERS)}）")
        params = {"Det.model_type": tier, "Rec.model_type": tier}
        try:
            from rapidocr import OCRVersion

            params["Det.ocr_version"] = OCRVersion.PPOCRV6
            params["Rec.ocr_version"] = OCRVersion.PPOCRV6
        except Exception:
            pass  # 老版本没这枚举：按它自己的默认版本走
        """
        关掉"文字行方向分类"（use_cls）。

        那一步是给扫描件准备的（纸放歪了、整行倒过来），屏幕截图里没有倒着的字行，
        却要对**每一行**都跑一次分类模型。实测同一张 640x360 的图：
            small  开 3.3s / 关 2.8s
            medium 开 13.9s / 关 13.4s
        省得不多（大头不在这儿），但没坏处、也少一个失败点；
        真要它就走 params.json 那条路：{"Global.use_cls": true}。

        顺便把三档在这台机器上的实测记在这（640x360、40 行上下，模型已下好）：
            tiny 2.6s   small 2.9s   medium 13.4s
        medium 准一些，但 CPU 上慢得多 —— 每一行都要过一次 73MB 的识别模型。
        命令最后那个词换成 small / tiny 就快回来了。
        """
        params["Global.use_cls"] = False
        return RapidOCR(params=params)
    with open(extra, "r", encoding="utf-8") as handle:
        return RapidOCR(params=json.load(handle))


def main(argv):
    args = argv[1:]
    if len(args) < 2:
        sys.stderr.write(
            "用法：ppocr_runner.py [tiny|small|medium 或 params.json] <图片> <结果.json>\n")
        return 2

    # 最后两个是 SmartClip 塞进来的（图片 / 结果），前面的是脚本自己的参数
    image_path, out_path = args[-2], args[-1]
    extra = args[0] if len(args) > 2 else None
    engine = _build_engine(extra)
    result = engine(image_path)

    boxes = getattr(result, "boxes", None)
    texts = getattr(result, "txts", None)
    lines = []
    if boxes is not None and texts is not None:
        for poly, text in zip(boxes, texts):
            if not text:
                continue
            lines.append({"text": str(text), "box": _poly_to_box(poly)})

    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(lines, handle, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
