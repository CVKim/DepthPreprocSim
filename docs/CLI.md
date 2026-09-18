> Detailed reference (Korean). Back to the [README](../README.md).

# CLI 사용법 (`build\Release\depth_sim.exe`)
```
depth_sim.exe run   --in <file> --out <dir> [--ref <file>] [옵션]
depth_sim.exe batch --folder <타이어 폴더> --out <dir> [--fovproc <FOVPROC.ini>] [옵션]
depth_sim.exe info  --in <file>
depth_sim.exe version
```
옵션(모두 선택): `--ini <alg_depth_preproc.ini> --cal <N>` (`[CAL000N]` 값을 기본값으로 로드, 이후 개별 옵션이 덮어씀),
`--type INNERCENTER|BEAD|INSHOULDER`(**지그 제거 규칙 선택**: 없음 / 최대 성분 / 침식 3 → 최대 성분 → 팽창; `--ini --cal` 을 주면 CAL 의 DepthPreprocType 이 기본),
`--break-kernel N`(INSHOULDER 침식 커널, 기본 3 = 현장 리터럴, 0~2 는 침식 없음 → `dll_identical=false`),
`--patch W,H`(기본 15,15), `--overlap F`(기본 0.25), `--lower P --upper P`(기본 5/95,
`--exp-use-ini-pct 1` 일 때만 적용), `--stage AUTO|TOP|BOTTOM|NONE`(기본 AUTO), `--roi x1,y1,x2,y2`, 실험 토글
`--exp-use-ini-pct --exp-valid-pct --exp-masked-median 0|1`, `--exp-null-value N`(-1=끄기), `--exp-fill-holes <maxpx>`(0=끄기),
`--dump min|all`(기본 all), `--preview N`(기본 8), `--px-x um --px-y um`(기본 300/100).

예시:
```bat
depth_sim.exe info --in "<images>\sampleA\7_SWU_Sh_R_D.mim"
depth_sim.exe run --in "...\7_SWU_Sh_R_D.mim" --ref "...\12_SWU_Sh_R_D_Proc.mim" --out runs\smoke7 --patch 15,15 --overlap 0.25 --stage AUTO
depth_sim.exe batch --folder "<images>\sampleB" --out runs\tire2
```
`run` 은 `<out>` 아래에 `result.png`, `result_f32.tif`, `raw_vis.png`, `null_mask.png`, `stage_removed_mask.png`, `scaled.png`,
`basis.png`, `diff.png`, `clipped.png`, (`--ref` 시) `ref.png`, `ref_diff.png`, 각 `preview_*.png`, `raw.f32 basis.f32 diff.f32`,
`stats.json` 을 만들고 `stats.json` 내용을 stdout 에 출력합니다. 종료 코드: 0 성공, 2 인자 오류, 3 입력 읽기 실패, 4 처리 예외.
오류는 stderr 에 한 줄 JSON `{"error":"..."}` 으로 출력합니다.

## 주의 사항
- **Overlap 기본값**: DLL 은 ini 에 `Overlap` 키가 없으면 0.5 를 쓰지만, CLI 는 ini 없이 실행할 때 **레시피 기본 0.25** 를 씁니다.
  `--ini` 로 ini 를 지정하면 DLL 과 동일하게 키가 없을 때 0.5 가 됩니다.
- **Lower/Upper Percentage**: DLL 은 읽기만 하고 5/95 를 하드코딩합니다. 시뮬레이터 기본 경로도 5/95 고정이며
  `--exp-use-ini-pct 1` 일 때만 지정값을 사용합니다(이 경우 `params_effective.dll_identical=false`).
- **StagePosition**: DLL 은 NONE(3)만 생략하고 AUTO/TOP/BOTTOM 은 모두 같은 동작입니다.
- **지그 제거 규칙은 타입별로 다릅니다(현장 빌드)**: INNERCENTER 는 제거 없음, BEAD 는 최대 8-연결 유효 성분만 유지
  (`removeStageFromRawDataBead`), INSHOULDER 는 3×3 사각 커널로 침식 → 최대 성분 선택 → 같은 커널로 팽창 → 원래 유효 마스크와 교집합
  (`removeStageFromRawData(src, stage, 3)`) — 단, 현장 코드의 얕은 복사 때문에 마지막 교집합이 **침식된 마스크**와 이루어져 실효 동작은
  "침식된 최대 성분만 유지" 입니다(§3.3). 폭 2 px 이하의 얇은 유효 조각·구멍 가장자리 1 px·띠 경계 1 px 가 함께 제거되고, 팽창 복원은
  일어나지 않습니다. `--type` 을 잘못 주면 결과가 현장과 달라지므로 가능하면 `--ini --cal` 로 로드하십시오.
- **ROI**: `x2>x1 && y2>y1` 일 때만 사용합니다. DLL 은 뷰를 `roi & imageRect` 로 자르지만 결과 복원은 자르지 않은 `roi` 를
  쓰기 때문에 ROI 가 이미지를 벗어나면 DLL 이 예외를 냅니다. 시뮬레이터도 같은 경우 exit 4 로 종료합니다.
- **8/16-bit 입력**: DLL 처럼 float 으로 변환만 합니다(값 스케일 변경 없음). 컬러 이미지는 그레이로 변환합니다.
- **result_f32.tif**: DLL 출력 버퍼 형식(float32, 값 0..255)의 단일 strip TIFF 입니다. MIL 로 다시 읽히는지는 보장하지 않습니다.
- **타이밍**: `timing_ms` 의 단계별 값은 캡처용 재실행에서 측정한 값이고, `total = read + remove_stage + scale + basis + diff + clip_normalize` 입니다.
  공식 결과는 항상 원본 `processHighCurvature` 호출로 만들며, 캡처 파이프라인과 바이트 단위로 비교합니다(`params_effective.capture_identical`).
- 실험 옵션(`exp_*`)이 하나라도 켜지면 결과는 DLL 과 달라지며 `stats.json` 의 `params_effective.dll_identical` 이 `false` 가 됩니다.

# Experimental options (not in any DLL)

Any of these sets `params_effective.dll_identical = false` in `stats.json`.

| Option | Default | Effect |
|---|---|---|
| `--exp-valid-pct 0\|1` | 0 | clip percentiles computed on valid pixels only |
| `--exp-masked-median 0\|1` | 0 | patch median ignores nulls |
| `--exp-use-ini-pct 0\|1` | 0 | use `--lower/--upper` instead of the hard-coded 5 / 95 |
| `--exp-stage-restore 0\|1` | 0 | INSHOULDER stage removal as the source comments intend (see VERIFICATION) |
| `--exp-abs-mm F` | 0 | fixed physical scale, +-F mm -> 1..255, 0 reserved for null |
| `--exp-null-value N` | -1 | output value for nulls (-1 = 0 like the DLL) |
| `--exp-fill-holes N` | 0 | fill null components up to N px before processing |
| `--algo site\|v2` | site | `v2` replaces the whole pipeline with the band-aware algorithm below |

## `--algo v2` modes

| `--v2-mode` | What it does |
|---|---|
| `neutral` | unreliable / null pixels become mid-grey (128), fixed scale `--v2-range-mm` |
| `restore` | measured pixels kept, nulls inside the band interpolated (multi-scale normalized convolution), slope correction, percentile contrast with `--v2-norm pct` |
| `restore2` | `restore` + dropout zones: wrong-valued clusters are rejected against a rotation-registered surface and holes are refilled in the residual domain |

Recommended call for `restore2`:

```bat
depth_sim.exe run --in <raw.mim> --out <dir> --ini alg_depth_preproc.ini --cal 3 --type INSHOULDER ^
    --algo v2 --v2-mode restore2 --v2-norm pct --v2-edge 1
```

| restore2 option | Default | Meaning |
|---|---|---|
| `--v2-zone-w / --v2-zone-h` | 401 / 21 | null-density window that defines a dropout zone (columns / rows) |
| `--v2-zone-thr` | 0.04 | density above which a pixel belongs to a zone |
| `--v2-zone-margin` | 30 | zone growth in px |
| `--v2-rm-win / --v2-rm-step` | 301 / 16 | running median along the rotation axis: window / evaluation step (columns) |
| `--v2-zone-seed-mm` | 0.6 | residual up to this is "on the surface" (seed) |
| `--v2-zone-grow-mm` | 0.35 | residual step between neighbours that still counts as continuous (0 = plain threshold) |
| `--v2-zone-out-mm` | 1.2 | gate for the surface correction (x2) and the threshold when growth is off |
| `--v2-zone-out2-mm` | 0.5 | reject when further than this from the local 5x5 median (0 = off) |
| `--v2-zone-norm 0\|1` | 1 | contrast percentiles from pixels outside the zones only |
| `--v2-slope 0\|1` | 1 | slope correction (residual x cos theta) |
| `--v2-edge N` | 2 | drop N px of measured data next to nulls (outside zones) |

Extra outputs of `restore2`: `zone_mask.png`, `rejected_mask.png`, `hole_mask.png` (refilled pixels); with `--dump all` also
`surface.f32` (robust surface, mm) and `restored.f32` (depth after refill, mm).
