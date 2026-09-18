# DepthPreprocSim 검증 결과 (tests/verify.py)

- 실행 시각: 2026-09-18 14:45:22
- 실행 파일: `E:\Dev\DepthPreprocSim\build\Release\depth_sim.exe`
- 실행 파일 `version` 출력: `depth_sim 1.1.0 (alg_depth_preproc site PC3 2026-07-08 rc 1.0.2.0.38cf930_HT, OpenCV 4.4.0)`
- 케이스 정의: `E:\Dev\DepthPreprocSim\tests\cases.json`
- 선택 케이스: T1, T2, T3, T4, T5
- 총 소요 시간: 112.5 s
- 전체 판정: **PASS**

## 요약

| 케이스 | 판정 | 설명 |
|---|---|---|
| T1 | PASS | tire1 default params per CAL vs site _Proc: exact/within1/within2/max_abs/black-mask IoU; cross-check with exe stats.json ref block |
| T2 | PASS | same shoulder inputs with --stage NONE: exact_pct must drop >= 20 pp vs T1 (removeStage sanity) |
| T3 | PASS | exe result vs python reference simulate.py withStage path (remove_stage + process_high_curvature) |
| T4 | PASS | tire2 batch with --fovproc: 8 item folders + batch.json; per-item metrics vs _Proc; individual runs must equal batch outputs |
| T5 | PASS | CLI error paths: missing input -> exit 3 + stderr JSON; bad --patch/--overlap -> exit 2 |

측정 정의: exact = 화소값 동일 비율, ±1/±2 = |result-ref| 가 1/2 이하인 비율, 검정 IoU = (result==0)∩(ref==0) / (result==0)∪(ref==0), 검정 일치 % = 두 영상의 검정(0) 여부가 같은 화소 비율, 참조만 검정 = ref==0 이고 result!=0 인 화소 수, 결과만 검정 = 그 반대. 교차검증 = 위 값과 exe 의 stats.json `ref` 블록 일치 여부(허용 0.01%).

## T1 tire1 기본 파라미터 vs 현장 _Proc — PASS

tire1 default params per CAL vs site _Proc: exact/within1/within2/max_abs/black-mask IoU; cross-check with exe stats.json ref block

| 쌍(raw→ref) | 종류 | CAL | exact % | ±1 % | ±2 % | max\|Δ\| | 검정 IoU | 검정 일치 % | 참조만 검정 | 결과만 검정 | 교차검증 | 파라미터 | exe 시간(s) | 판정 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5_11 | shoulder | 3 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 3.4 | PASS |
| 7_12 | shoulder | 3 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 3.2 | PASS |
| 17_23 | shoulder | 3 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 3.4 | PASS |
| 19_24 | shoulder | 3 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 3.1 | PASS |
| 3_10 | bead | 2 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 1.4 | PASS |
| 1_9 | center | 1 | 100.000 | 100.000 | 100.000 | 0 | 1.0000 | 100.000 | 0 | 0 | 일치 | 일치 | 6.9 | PASS |

## T2 --stage NONE 대비 하락(removeStage 확인) — PASS

same shoulder inputs with --stage NONE: exact_pct must drop >= 20 pp vs T1 (removeStage sanity)

| 쌍 | T1(AUTO) exact % | NONE exact % | 하락(pp) | AUTO↔NONE 동일 % | 지그 제거 화소 AUTO / NONE | NONE 검정 IoU | 판정 |
|---|---|---|---|---|---|---|---|
| 5_11 | 100.000 | 50.591 | 49.41 | 50.59 | 1188069 / 0 | 0.8367 | PASS |
| 7_12 | 100.000 | 43.590 | 56.41 | 43.59 | 2638884 / 0 | 0.6767 | PASS |
| 17_23 | 100.000 | 50.306 | 49.69 | 50.31 | 1223222 / 0 | 0.8332 | PASS |
| 19_24 | 100.000 | 43.929 | 56.07 | 43.93 | 2652101 / 0 | 0.6772 | PASS |

## T3 파이썬 레퍼런스(withStage) 대비 — PASS

exe result vs python reference simulate.py withStage path (remove_stage + process_high_curvature)

| 쌍 | exe↔py exact % | ±1 % | ±2 % | 검정 IoU | py↔현장 exact % | py↔현장 ±2 % | py↔현장 검정 IoU | py 시간(s) | 판정 |
|---|---|---|---|---|---|---|---|---|---|
| 5_11 | 99.985 | 100.000 | 100.000 | 1.0000 | 99.985 | 100.000 | 1.0000 | 2.2 | PASS |
| 7_12 | 99.988 | 100.000 | 100.000 | 1.0000 | 99.988 | 100.000 | 1.0000 | 1.9 | PASS |
| 17_23 | 99.985 | 100.000 | 100.000 | 1.0000 | 99.985 | 100.000 | 1.0000 | 2.1 | PASS |
| 19_24 | 99.987 | 100.000 | 100.000 | 1.0000 | 99.987 | 100.000 | 1.0000 | 2.1 | PASS |
| 3_10 | 99.979 | 100.000 | 100.000 | 1.0000 | 99.979 | 100.000 | 1.0000 | 0.8 | PASS |
| 1_9 | 99.994 | 100.000 | 100.000 | 1.0000 | 99.994 | 100.000 | 1.0000 | 3.3 | PASS |

## T4 tire2 batch(--fovproc) + 개별 run 동일성 — PASS

tire2 batch with --fovproc: 8 item folders + batch.json; per-item metrics vs _Proc; individual runs must equal batch outputs

- batch 종료 코드: 0, 소요 27.0 s, batch.json 항목 8, 결과 폴더 8개

| 폴더 | imgIdx→ref | CAL | exact % | ±1 % | ±2 % | 검정 IoU | 검정 일치 % | 교차검증 | 파라미터 | 판정 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1_SWU_Cen_D | 1→9 | 1 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 3_SWU_Bead_D | 3→10 | 2 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 5_SWU_Sh_L_D | 5→11 | 3 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 7_SWU_Sh_R_D | 7→12 | 3 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 13_SWD_Cen_D | 13→21 | 1 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 15_SWD_Bead_D | 15→22 | 2 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 17_SWD_Sh_L_D | 17→23 | 3 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |
| 19_SWD_Sh_R_D | 19→24 | 3 | 100.000 | 100.000 | 100.000 | 1.0000 | 100.000 | 일치 | 일치 | PASS |

개별 `run` 과 batch 결과 동일성:

| 쌍 | exact % | 검정 IoU | batch 와 동일 | 시간(s) | 판정 |
|---|---|---|---|---|---|
| 5_11 | 100.000 | 1.0000 | True | 2.7 | PASS |
| 7_12 | 100.000 | 1.0000 | True | 2.9 | PASS |
| 17_23 | 100.000 | 1.0000 | True | 2.8 | PASS |
| 19_24 | 100.000 | 1.0000 | True | 3.0 | PASS |
| 3_10 | 100.000 | 1.0000 | True | 1.6 | PASS |
| 1_9 | 100.000 | 1.0000 | True | 6.9 | PASS |

## T5 CLI 오류 경로 — PASS

CLI error paths: missing input -> exit 3 + stderr JSON; bad --patch/--overlap -> exit 2

| 시나리오 | 기대 exit | 실제 exit | stderr JSON | 판정 |
|---|---|---|---|---|
| missing_input | 3 | 3 | `{"error": "input not found: E:\\Dev\\DepthPreprocSim\\tests\\__does_not_exist__\\nofile_D.mim"}` | PASS |
| bad_args_1_patch | 2 | 2 | `{"error": "invalid --patch '0,15'; expected W,H with positive integers"}` | PASS |
| bad_args_2_overlap | 2 | 2 | `{"error": "invalid --overlap; expected range (0, 1)"}` | PASS |

