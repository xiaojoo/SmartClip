# RapidOCR (RapidAI) as an Embedded OCR Engine — Windows / Python+Qt

## 1. What it is
- **[RapidOCR](https://github.com/RapidAI/RapidOCR)** is an open-source OCR toolkit by **RapidAI** (lead maintainer SWHL; PyPI owner org `RapidAI`) that converts PaddleOCR's PP-OCR **det / cls / rec** models to **ONNX** and runs them on ONNX Runtime (default), OpenVINO, MNN, PaddlePaddle, TensorRT or PyTorch — deliberately without the PaddlePaddle framework.
- **License: Apache-2.0** (`license_expression: Apache-2.0` on PyPI); converted weights remain upstream PaddleOCR/Baidu Apache-2.0 and are redistributed under the same terms.
- **Current version: `rapidocr` 3.9.2 (2026-07-21)**. Cadence is roughly monthly: 3.6.0 (Jan 2026), 3.7.0 (Mar), 3.8.0 (Apr), 3.9.0 (Jun), 3.9.2 (Jul).
- **Model versions:** `rapidocr >= 3.9.0` defaults to **PP-OCRv6** (det `small`, rec `small`, cls PP-OCRv4 mobile); `< 3.9.0` defaulted to **PP-OCRv4** mobile. **PP-OCRv4, v5 and v6 all ship and remain selectable** via `OCRVersion`/`ModelType`.

## 2. Installation
- Canonical: `pip install rapidocr onnxruntime` — Python `>=3.8,<4` (classifiers 3.8–3.13).
- **`rapidocr-onnxruntime` (1.4.4), `rapidocr-openvino` (1.4.4), `rapidocr-paddle` (1.4.5)** — all last released Jan 2025 and **officially deprecated**: the docs warn the three are "gradually no longer maintained" and were merged into `rapidocr`, which selects engines via `EngineType`.
- **ONNX Runtime is no longer a dependency since `rapidocr` 2.0.6** — you install your own engine.
- **Weight: the `rapidocr` wheel is ~27.2 MB and bundles the three default models**; all other models live on [ModelScope](https://www.modelscope.cn/models/RapidAI/RapidOCR/files) and auto-download on first use.
- Other deps: opencv-python, numpy, Shapely, pyclipper, PyYAML, Pillow, tqdm, omegaconf, requests, colorlog.
- Caveat: **onnxruntime 1.30.0 requires Python ≥3.11** (win_amd64 wheel **~14.3 MB**); pin an older ORT for Python 3.8–3.10.

## 3. Resources & performance
- Default ONNX model sizes: **det** tiny 1.8 MB / small 9.9 MB / medium 62.1 MB; **rec** tiny 4.5 MB / small 21.2 MB / medium 76.6 MB; **cls** 0.59 MB (v4) or 1.0 MB (v5 LCNet x0.25).
- **CPU speed (Apple M2, official benchmark):** full det+cls+rec ≈ **0.93–1.26 s/image → ~0.8–1.1 images/s**; native PaddleOCR CPU 1.75–1.93 s for the same image (~1.6–2× slower). Per module: det 0.23 s, rec 0.07 s. **No official Windows-desktop benchmark exists.**
- **RAM: not published.** Footprint is dominated by onnxruntime + OpenCV plus ~32 MB of default models; maintainer notes OpenVINO is faster but "uses more memory".
- Accelerators: **CUDA / TensorRT** (rapidocr ≥3.7.0; v6 validated in 3.9.2), **OpenVINO**, **MNN** (mobile), PyTorch, Paddle, and **DirectML** (`onnxruntime-directml`, Windows 10 1903+). Known caveat: onnxruntime-gpu can be *slower* than CPU because OCR inputs are dynamic-shape.

## 4. Capabilities
- **OCR only**: detection + line-orientation classification + recognition, ~80 language codes (ch, chinese_cht, en, japan, korean, latin, cyrillic, arabic, devanagari, ta, te, th, el, eslav…).
- **No layout, table, formula or reading-order logic inside `rapidocr`.** Separate packages: **`rapid-layout`** (layout analysis, 1.2.1), **`rapid-table`** (table recognition, 3.0.2), **`rapid-doc`** (PDF→Markdown/JSON/DOCX/HTML pipeline: layout + formula + table + reading order, 0.9.10), plus RapidLaTeXOCR, RapidOrientation, RapidUnWrap, RapidTableDetection.

## 5. Accuracy
- Same weights as PaddleOCR; ONNX conversion is numerically faithful — det/rec metrics were **identical across onnxruntime/OpenVINO/Paddle engines** in the project's own eval.
- Project eval: PP-OCRv4 mobile rec exact-match **0.832**/char 0.936 vs PP-OCRv5 mobile rec **0.736**/0.918 on their test set — "newer" is not uniformly better; server models are far larger and stronger.
- Third-party (Guangzhou Software Institute, 12 open-source OCR tools): RapidOCR placed well overall and **beat native PaddleOCR on 180°-rotation and low-contrast** cases, credited to preprocessing/parameter tuning.
- **doc-VLMs:** RapidDoc (pipeline, PP-OCRv6-small) scores **90.16** overall on OmniDocBench v1.6 vs 94–96 for specialized VLMs (MinerU2.5-Pro 95.75, PaddleOCR-VL 94.18) — the gap is largest on tables (TEDS 81.4 vs 93.4).
- Weak points: handwriting, arbitrary rotation (cls is 0/180° only), dense multi-column layouts, low-res scans; the `padding` parameter is the documented mitigation.

## 6. Windows / offline / Qt
- Windows is first-class: an official **prebuilt `RapidOCRWeb.exe` desktop zip** exists, with PyInstaller/Nuitka packaging docs. Known issues: Shapely `WinError 126` install failure; **no Windows 7 support**.
- **Offline: fully supported** — set `model_path`/`model_dir` per module (det/cls/rec) plus the dict file; all models are downloadable from ModelScope.
- **C++ precedent: [RapidOcrOnnx](https://github.com/RapidAI/RapidOcrOnnx)** (ONNX Runtime C++ + OpenCV 4.8.1) ships a **C dynamic library (`clib`)** and JNI libs — callable from Qt through that C API, but it is **stale** (ORT 1.15.1, PP-OCRv3, last update Jan 2024). C# WinForms sample: RapidOCRCSharp.
- **Qt precedent: [ocr-qt-gui](https://github.com/whitexiong/ocr-qt-gui)** — a PySide6 desktop app advertising RapidOCR integration, with threaded OCR, SQLite results and a PyInstaller spec; note its bundled runtime is actually `PaddleOCR-json.exe`, so treat it as a UI/packaging reference rather than a RapidOCR integration proof.
