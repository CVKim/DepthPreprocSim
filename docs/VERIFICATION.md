> Detailed reference (Korean). Back to the [README](../README.md).

# 검증 결과 (2026-09-18, Release 빌드, v142 14.29.30133, OpenCV 4.4.0, 기준 = 현장 PC3 소스 2026-07-08)

`python tests\verify.py --exe build\Release\depth_sim.exe --cases tests\cases.json --out tests\results` 결과(`tests\results\RESULTS.md`).
전체 판정 **PASS**(T1~T5), UI 스모크 12/12 PASS.

## 현장 `_Proc` 대비 — 14장 모두 100 % 비트 동일
| 케이스 | 표본 | 이미지 | exact % | 불일치 px | 검정 IoU |
|---|---|---|---|---|---|
| T1 | 정상 표본 A | 숄더 5→11, 7→12, 17→23, 19→24 / 비드 3→10 / 센터 1→9 | **100.000** ×6 | 0 | 1.0000 |
| T4 (batch, `--fovproc --ini`) | 이상 표본 B | 1, 3, 5, 7, 13, 15, 17, 19 → 9, 10, 11, 12, 21, 22, 23, 24 | **100.000** ×8 | 0 | 1.0000 |

exe 의 `stats.json` `ref` 블록과 `verify.py` 의 독립 계산이 0.01 % 이내로 일치(교차검증)하고, `batch` 결과와 개별 `run` 결과도 동일합니다.
처리 시간은 현장 로그(9/14 PC3, 58회 중앙값)와 같은 수준입니다: 숄더 1.35 s(현장 1.32), 센터 2.95 s(3.00), 비드 0.56 s(0.62).

## 지그 제거 확인 (T2)
같은 숄더 입력을 `--stage NONE` 으로 돌리면 exact 가 100 → 43.9~50.6 % 로 떨어집니다(하락 49~56 pp). 현장 DLL 에 지그 제거가 들어 있고
시뮬레이터가 같은 규칙을 쓰는 것을 뜻합니다.

## 현장 소스와 talos-platform 소스의 차이, 그리고 현장 코드의 실제 동작
현장 소스는 talos-platform 커밋 0a6814f2(feat/ALG-288) 와 **지그 제거 단계만** 다릅니다(내용 차이 65줄, `reference/site_pc3_20260708/`).

| 타입 | 현장 PC3 (2026-07-08) | 0a6814f2 |
|---|---|---|
| INSHOULDER | `removeStageFromRawData(src, stage, 3)`: 3×3 침식 → 최대 성분 → 팽창 → 마스크 교집합 | 최대 성분만 유지 |
| BEAD | `removeStageFromRawDataBead`(= 구 최대 성분 유지) | 최대 성분만 유지 |
| INNERCENTER | 제거 없음(호출 주석) | 최대 성분 유지 |
| 디버그 `MbufSave(E:\debug_input_after_proc.mim)` | 주석 처리 | 활성 |

**주의 — 현장 코드의 실제 동작은 주석의 의도와 다릅니다.** `cv::Mat labelMask = mask;` 가 얕은 복사이고 `cv::erode(mask, labelMask, kernel)`
이 제자리(in-place)로 수행되어 `mask` 자체가 침식됩니다. 그래서 마지막 `bitwise_and(keep, mask)` 는 침식된 마스크와의 교집합이 되고,
"팽창으로 경계 복원" 은 일어나지 않습니다. 실효 규칙은 **침식된 최대 성분만 유지** 이며, 띠 경계와 모든 구멍 가장자리가 1 px 씩
추가로 검게 됩니다(정상 표본 7→12 에서 39,662 px, dropout 이 많은 이상 표본에서는 수십만 px). 파이썬으로 두 해석을 비교한 결과
의도대로(복원) 구현하면 exe 와 99.75 % 만 일치하고, 실효 규칙(침식 마스크 교집합)으로 구현하면 100 % 일치합니다. 시뮬레이터는 현장 소스를
그대로 복사했으므로 현장과 같은 동작을 하며, `tests/reference/simulate.py` 의 `remove_stage(restore=True)` 가 의도된 동작입니다.
이 동작을 바꾸려면 현장 코드 수정이 필요합니다.

## 파이썬 레퍼런스 대비 (T3)
`tests/reference/simulate.py`(numpy/cv2 독립 구현, 타입별 실효 규칙 포함) 와 exe 결과는 6쌍 모두 exact 99.979~99.994 %, ±1 이내 100 %,
검정 IoU 1.0000 입니다(남는 차이는 float32/double 반올림).

## CLI 계약 확인 (T5)
없는 입력 → exit 3 + stderr JSON, `--patch 0,15` / `--overlap 1.5` → exit 2. 이미지를 벗어나는 ROI → exit 4(DLL 과 같은 실패 지점).
`--ini ... --cal N` 으로 CAL 의 PatchSize/Overlap/DepthPreprocType 이 로드되고, `--type` 이 지그 제거 규칙을 바꿉니다
(`--type BEAD` 나 `--break-kernel 0` 으로 숄더를 돌리면 100 % 가 깨지고 `dll_identical` 이 false 가 됩니다).

## 성능 참고
처리 자체는 숄더 1장 약 1.35 s, 센터 약 2.95 s 입니다. `--dump all` 산출물(약 250 MB)을 E: 에 쓰면 수십 초가 더 걸리므로 빠르게 반복할 때는
`--dump min` 또는 빠른 디스크의 `--out` 을 사용하십시오. 상대 `--out` 은 `Result\runs\` 아래로 해석됩니다(절대 경로 권장).

# 검증 로그
2026-09-18 (TASK A, C++ core/CLI)
```
build.bat Release                      -> build\Release\depth_sim.exe (MSVC 19.29.30159 = v142, /O2 /Oi /GL /LTCG, opencv_world440.dll 복사, MD5 원본과 동일)
cmake --build build --config Debug     -> build\Debug\depth_sim.exe (assert 활성) 정상 실행, result.png MD5 = Release
depth_sim info --in ...\7_SWU_Sh_R_D.mim         8192x1940 float32, null 5321004 (33.48%), valid -53.1895 / 18.6545
depth_sim run  7_SWU_Sh_R_D --ref 12_..._Proc --patch 15,15 --overlap 0.25 --stage AUTO   -> runs\smoke7
   ref.exact_pct 65.12  within1 90.31  within2 98.08  black_mask_iou 0.98780  capture_identical true  timing total 1317 ms
depth_sim run  ... --stage NONE                  -> exact 43.59  within2 54.30  iou 0.6767 (T2: 크게 나빠짐 확인)
depth_sim batch --folder ...\sampleB --out runs\tire2_min --dump min   -> exit 0, 8장, BEAD 2장 exact 100.00 / IoU 1.0
depth_sim batch ... --fovproc FOVPROC.ini --ini alg_depth_preproc.ini             -> exit 0, 매핑 1→9,3→10,5→11,7→12,13→21,15→22,17→23,19→24 / CAL 1,2,3
오류 경로: 없는 파일 exit 3 / --patch 15x15 exit 2 / --overlap 1.5 exit 2 / --out 누락 exit 2 / 이미지 밖 ROI exit 4
```
