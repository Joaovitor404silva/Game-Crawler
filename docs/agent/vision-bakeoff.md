# Vision bake-off (V4), 2026-11-11

Tools: `tools/agent_vision.py` (signals), `tools/agent_vision_bench.py` (latency + fixed-Hz loop for game cost),
`tests/agent/test_vision.py` (synthetic bars, JPEG/scale, profile parse, OCR; `--gpu` adds DirectML).
Setup: `py -m venv tools\.venv-vision` + `pip install numpy opencv-python-headless onnxruntime-directml rapidocr`.

Measured on Bloodborne (bbport RT, 3172x1328 DLSS, 118 FPS idle). Not yet measured on S2.

## Results

| Signal | Backend | p50 / p95 ms | Loop Hz reached | Game FPS cost | Accuracy | Verdict |
|---|---|---|---|---|---|---|
| HUD hp/stamina (bar fill) | numpy | 0.22 / 0.43 | 5.0 (target) | none measurable | max error 1.2% vs memory hp (independent sample); synthetic 0.3% | **keep as fallback** (memory first) |
| prompts/menus (RapidOCR) | CPU onnxruntime | 514 / 556 | 1.96 | **-20%** | synthetic prompts exact | cut (fails FPS <= 5%) |
| prompts/menus (RapidOCR) | GPU DirectML | 742 / 1034 | 1.38 | none measurable | same models, same output | **keep, on demand only** |
| moving / stuck | - | - | - | - | - | cut: agentcore has it from positions |
| free space ahead | - | - | - | - | - | cut: no consumer |
| entities (detector) | - | - | - | - | - | on hold: hosts read entities from memory (BB + UE) |

Thresholds from the plan: HP error <= 5%, game FPS drop <= 5%. HUD passes both. GPU OCR passes the FPS gate;
CPU OCR does not. Neither OCR backend reaches 5 Hz, so OCR is never a loop: call it when an agent needs to read text.

## Decisions

- Memory beats vision. The Bloodborne host now publishes hp/stamina/position/entities from memory, so the HUD probe is
  only for hosts without those fields. Its regions live in the game profile (`hud_hp`, `hud_stamina`); the bar frame
  grows with the character's stats, so re-measure `x1` per character.
- OCR runs on the GPU backend by default, on demand, cropped to `prompt_region`.
- Capture stays in agentd (WGC, 16-20 ms); Python reads saved frames, so no windows-capture/dxcam dependency.
- Not wired into agentd events: nothing needs them while memory fields exist. Revisit for a vision-only game.

## Auto-labelled dataset (V2, `tools/agent_vision_dataset.py`)

`record` drives the player through agentd (attacks, dodges, heals) and saves a clean frame (`look overlay=0`) with
the memory hp/stamina read before and after each capture; `score` runs `hud` on every frame.

| Pass | Frames | HP mean / max error | Stamina mean / p95 / max | Stamina within 5% |
|---|---|---|---|---|
| 1 (in a fight, hp pinned low) | 200 (15 stamina skipped: changed mid-capture) | 0.3% / 0.3% | 2.2% / 6.0% / 11.4% | 89% |
| 2 (heal at < 60%) | 120 | 1.1% / 1.7% | 2.4% / 5.8% / 19.6% | 88% |
| 3 (mid range, 2026-11-12: blood bullets step hp -30%, a vial +40%) | 120 | 0.4% / 1.3% | 3.7% / 11.0% / 11.9% | 87% |

Pass 3 covers 8 levels: 10, 20, 31, 40, 51, 61, 70 and 71%. Passes 1-2 covered 0.2, 40 and 78%. The first score of
pass 3 was 87.5% within 5%, with errors up to 29%. The cause: right after hp drops, Bloodborne shows the lost part as
an orange regain (rally) segment after the white tick. That orange falls inside the hp hue range, and hue alone cannot
separate it, because real hp also reaches hue 6-10 in other frames. The fix is in `bar_fill`: a white tick inside the
filled run ends the fill. After the fix, all three passes are 100% within 5%. A synthetic test checks this: the rally
case fails at 30% error without the rule and passes at 0.3% with it. Stamina labels are clamped to 0..1: Bloodborne stamina
goes negative after an action while the bar shows empty. The remaining stamina error is the bar animating behind
the memory value.

## OCR on real frames

7 Bloodborne frames, hand-labelled: intro logo, title, main menu, 4 pause-menu screens (27 phrases). Full frame,
no `prompt_region`.

| Backend | Native size: found / median ms | 2x upscale (now default under 1280 px): found / median ms |
|---|---|---|
| GPU DirectML | 26/27 / 114 | **27/27** / 125 |
| CPU | 26/27 / 239 | **27/27** / 285 |

The native-size miss was "Arms": a small label sitting on a decorative line. A larger detector input
(`Det.limit_side_len` 1280/1920) also finds it, but costs 221/400 ms and adds junk boxes; the 2x cubic upscale is
cheaper and clean. Boxes are mapped back to frame pixels. The profile key `ocr_scale` overrides the scale. Menu labels
sit above `prompt_region`, so pass a full frame for menus.
(The earlier 227/450 ms run was measured on a busier machine. Both columns here come from the same run.)

## Open

- S2 numbers (HUD regions + OCR cost on the dev pair): parked.