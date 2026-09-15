# IBM Granite-Docling-258M — Research Report (embedded doc/image OCR engine)

All key claims cited. **[C]** = confirmed by IBM/upstream primary source. **[E]** = third-party estimate, not vendor-verified.

## 1. What it is [C]
- **Architecture**: Idefics3 base, with two swaps — vision encoder `google/siglip2-base-patch16-512`, language model **Granite 165M**; connector is the idefics3 pixel-shuffle projector. Trained with HuggingFace `nanoVLM`. ([model card](https://huggingface.co/ibm-granite/granite-docling-258M))
- **Parameters/size**: 258M. Repo weights `model.safetensors` = **515,093,104 bytes (~515 MB)** at bf16 ([HF API](https://huggingface.co/api/models/ibm-granite/granite-docling-258M?blobs=true)).
- **License**: Apache-2.0 ([IBM announcement](https://www.ibm.com/new/announcements/granite-docling-end-to-end-document-conversion), model card).
- **Release date**: model card says **September 17, 2025**; IBM announcement published **24 September 2025**. (Discrepancy is real — cite both.)
- **DocTags**: IBM Research markup for image→sequence tasks; separates textual content from structure, uses `<loc_*>` coordinate tokens, and is converted to Markdown/HTML/JSON via `docling-core`. IBM states it is optimized for LLM readability and avoids Markdown/HTML's lossiness for tables/forms/math ([IBM](https://www.ibm.com/new/announcements/granite-docling-end-to-end-document-conversion)).

## 2. Hardware / resources
- **bf16 VRAM**: `~1.2–1.5 GB` bf16, `~2–3 GB` fp32, CPU-only viable at low throughput [E] ([llm.co](https://llm.co/llms/granite-docling-258m)). A vendor blog estimates `~0.5 GB` FP16 / `~0.3 GB` INT8 decode footprint and **~80 pg/min peak decode on L40S, ~100 pg/min on A100 80G**, explicitly labelled architecture-based decode-only estimates with sustained ≈ half [E] ([Spheron](https://www.spheron.network/blog/best-open-source-ocr-vlm-self-host-gpu-cloud-2026/)). A community post claiming 8–12 GB is not credible for a 515 MB bf16 checkpoint [E] ([HF discussion 27](https://huggingface.co/ibm-granite/granite-docling-258M/discussions/27)).
- **CPU**: Docling's spec declares CPU support for the Transformers variant [C] ([vlm_model_specs.py](https://github.com/docling-project/docling/blob/main/docling/datamodel/vlm_model_specs.py)); expect seconds-to-minutes per page — Docling's own table shows CPU VLM inference at 1175 s/page for Phi-4, so CPU is not production-viable for volume ([vision_models.md](https://github.com/docling-project/docling/blob/main/docs/usage/vision_models.md)).
- **Quantized**: no official int8/GGUF. Community only: GGUF `Q6_K` 171 MB, `IQ4_XS` 124 MB + `mmproj` 190 MB ([GGUF repo](https://huggingface.co/SandLogicTechnologies/granite-docling-258M-GGUF)); ONNX (int8/uint8/q4/fp16) from `onnx-community` ([ONNX repo](https://huggingface.co/onnx-community/granite-docling-258M-ONNX)); MLX 631 MB ([MLX repo](https://huggingface.co/ibm-granite/granite-docling-258M-mlx)). llama.cpp/Ollama support is community-contributed, not IBM.

## 3. Benchmarks [C]
IBM publishes **no OmniDocBench score** — the model card reports its own docling-eval/lmms-eval numbers only, and Spheron likewise lists OmniDocBench v1.6 as "N/A" for Granite-Docling ([model card](https://huggingface.co/ibm-granite/granite-docling-258M), [Spheron](https://www.spheron.network/blog/best-open-source-ocr-vlm-self-host-gpu-cloud-2026/)). Anything quoting an OmniDocBench figure for this model is unverified.

Model-card numbers (Granite-Docling vs SmolDocling): layout MAP 0.27/0.23, F1 0.86/0.85; full-page OCR edit-distance 0.45/0.48, F1 0.84/0.80; **code** edit-distance 0.013/0.114, F1 0.988; **equations** edit-distance 0.073/0.119, F1 0.968; **FinTabNet 150 dpi tables** TEDS-structure 0.97 vs 0.82, TEDS-content 0.96 vs 0.76; OCRBench 500 vs 338. Handwriting is **not** claimed or benchmarked; a third party states the model "is not designed for handwriting" [E].

## 4. Integration
- **pip**: `docling` (2.127.0, requires Python >=3.10,<4.0, classifiers include Windows/macOS/Linux); `docling-ibm-models` is only the TableFormer/layout package, *not* the VLM — the VLM runs via `transformers`, `vllm`, `mlx-vlm`, or ONNX [C].
- **CLI**: `docling --pipeline vlm --vlm-model granite_docling FILE` [C].
- **Weight download**: ~515 MB bf16 + tokenizer [C].
- **Batch**: yes — batch inference is an official pattern via vLLM `llm.generate([...])` [C]. Docling's vLLM/OpenAI-compatible option sets `concurrency=4` [C].
- **Offline bundling**: `artifacts_path=`, `--artifacts-path=`, `DOCLING_ARTIFACTS_PATH`, plus HF cache prefetch ([advanced_options.md](https://github.com/docling-project/docling/blob/main/docs/usage/advanced_options.md)).
- **Serving gotchas [C]**: vLLM breaks on tied weights → use `--revision untied`; pre-bf16 GPUs (T4) emit `!!!!` → `--dtype float32`.

## 5. Limitations
- Docling sets `scale=2.0`, `temperature=0.0`, `max_new_tokens=8192` with stop strings `</doctag>`/`<|end_of_text|>` — a **per-page** cap; dense pages can truncate [C].
- It consumes a page **image**; Docling's `VlmPipeline` rasterizes PDFs itself, so no separate layout model is required (IBM: "replacing traditional OCR and layout pipelines") [C]. An optional layout-guided variant exists: `docling-project/granite-docling-2stage-258m` [C].
- Languages: English primary; Arabic/Chinese/Japanese **experimental and unvalidated** [C].
- Community complaints: `<!DOCTYPE`-style degenerate output `…<loc_0><loc_0><loc_499><loc_500>` ([issue #2398](https://github.com/docling-project/docling/issues/2398)); DocTags not parsed from a vLLM response ([#2868](https://github.com/docling-project/docling/issues/2868)); poor extracted-image resolution ([#2416](https://github.com/docling-project/docling/issues/2416)); "disappointing quality and repeating loops on scanned documents", with IBM staff replying that looping usually signals subtly wrong input formatting ([GGUF discussion 1](https://huggingface.co/ibm-granite/granite-docling-258M-GGUF/discussions/1)).

## 6. vs PaddleOCR-VL and RapidOCR — deployment cost
- **PaddleOCR-VL (0.9B)** [C]: Apache-2.0, NaViT dynamic-resolution encoder + ERNIE-4.5-0.3B, **109 languages**, OmniDocBench 94.5% (v1.5) / 96.3% (v1.6) ([model card](https://huggingface.co/PaddlePaddle/PaddleOCR-VL), [vLLM recipe](https://recipes.vllm.ai/PaddlePaddle/PaddleOCR-VL)). ~3.5× the parameters; Spheron estimates ~2 GB FP16 and ~45 pg/min L40S [E] — so roughly 1.7–2× Granite-Docling's cost per page but far better multilingual accuracy.
- **RapidOCR** [C]: Apache-2.0, PaddleOCR models converted to **ONNX**, CPU-first via `onnxruntime` (also GPU/TensorRT/OpenVINO), Python/C++/Java/C#, no GPU needed ([README](https://github.com/RapidAI/RapidOCR)). Cheapest to deploy by far, but it is classical detect+recognize OCR — no layout hierarchy, table structure, or LaTeX — so it is not a like-for-like replacement for DocTags output.
- **Net**: Granite-Docling is the lowest-compute option that yields structured output; PaddleOCR-VL wins on accuracy/languages; RapidOCR wins on deployment cost and footprint when only text lines are needed.
