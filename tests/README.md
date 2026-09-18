# 검증 하네스 (tests/)

이 문서는 루트 `README.md` 의 "검증" 절에 해당한다. `DESIGN.md` §5 의 검증 계약(T1~T6)을 `tests/verify.py`(T1~T5) 와
`tests/ui_smoke.py`(T6) 로 구현하였다. 모든 산출물은 `tests/results/` 아래(ASCII 경로)에 생성되며, H: 의 현장 데이터는 읽기만 한다.

## 1. 구성 파일

| 파일 | 역할 |
|---|---|
| `tests/cases.json` | 케이스 정의. 데이터 폴더(tire1/tire2)·레시피 ini·파이썬 레퍼런스의 **절대 경로**, raw→_Proc 쌍, CAL 별 기대 파라미터, 판정 임계값(`expect`) |
| `tests/verify.py` | T1~T5 실행기. exe 를 subprocess 로 호출하고 `result.png` 와 현장 `_Proc.mim` 을 직접 비교한 뒤 exe 의 `stats.json` `ref` 블록과 교차검증한다 |
| `tests/ui_smoke.py` | T6. `node ui/server.js` 를 기동하여 `/api/config`, `/api/run`, `/api/pixel`, `/runs/...`, `/api/batch` 응답을 검사하고 서버를 종료한다 |
| `tests/fake_exe.py` | C++ 빌드가 없을 때 하네스 자체를 검증하기 위한 **파이썬 스탠드인**. CLI 계약(§2)을 파이썬 레퍼런스 위에 구현한 것으로 DLL 과 비트 동일하지 않다 |
| `tests/results/` | `RESULTS.md`(케이스별 표), `results.json`(전체 수치), `T1/…`, `T2/…`, `T3/…`, `T4/…`, `T5/…`(exe 출력), `ui_smoke.json`, `ui_smoke_server.log` |

## 2. 사전 준비

- Python 3.12 (Anaconda, `D:\anaconda\python.exe`) + `numpy`, `tifffile`, `opencv-python`(cv2). 확인: `python -c "import numpy, tifffile, cv2"`
- Node v24 (`C:\Program Files\nodejs\node.exe`), npm 패키지 불필요.
- 빌드된 `depth_sim.exe`(예: `build\Release\depth_sim.exe`). 실행 파일 옆에 `opencv_world440.dll` 이 있어야 한다.
- 데이터: `H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060`(tire1),
  `…\PC3\2000026091715281105`(tire2). 레시피 `D:\AIV\MODEL\[1]TireInspect_PC3_DEPLOY\alg_depth_preproc.ini`, `FOVPROC.ini`.
  경로가 다르면 `tests/cases.json` 만 수정한다.

## 3. verify.py 실행

```
cd E:\Dev\DepthPreprocSim
D:\anaconda\python.exe tests\verify.py --exe build\Release\depth_sim.exe --cases tests\cases.json --out tests\results
D:\anaconda\python.exe tests\verify.py --exe build\Release\depth_sim.exe --only T1,T3        # 일부 케이스만
```

- `--exe` 가 없으면 안내 메시지를 출력하고 종료 코드 1 로 끝난다(크래시하지 않음). `.py` 를 넘기면 현재 파이썬으로 실행한다(스탠드인용).
- 종료 코드: 선택한 모든 케이스 PASS 이면 0, 그 외 1.
- 각 케이스는 독립적으로 try/except 로 보호되어 한 케이스의 예외가 다른 케이스를 막지 않는다(예외는 RESULTS.md 에 traceback 으로 기록).
- 소요 시간(참고, 스탠드인 기준): T1 약 40 s, T2 약 30 s, T3 약 70 s, T4 수 분, T5 1 s. exe 호출 타임아웃은 `cases.json` 의 `timeouts_sec`(run 600 s, batch 1800 s).
- T2·T3 은 같은 `--out` 아래의 `T1/<쌍>/result.png` 가 있으면 재사용하고, 없으면 exe 를 추가 실행한다.

### 3.1 케이스와 판정 기준

| 케이스 | 입력 | exe 명령 | PASS 조건 |
|---|---|---|---|
| T1 | tire1 숄더 4쌍(5→11, 7→12, 17→23, 19→24; CAL3 15,15/0.25), 비드 3→10(CAL2 15,15/0.25), 센터 1→9(CAL1 50,50/0.1) | `run --in raw --out d --ref proc --ini <ini> --cal N --dump min` | exit 0, `result.png`·`stats.json` 존재, exact ≥ `exact_min_pct`(99.9, 실측 100.000), 검정 마스크 IoU ≥ 0.99, 하네스 측정값과 `stats.json.ref` 가 0.01% 이내로 일치, `params_effective`(patch/step/overlap/lower/upper/stage) 가 CAL 정의와 일치하고 `dll_identical=true` |
| T2 | T1 숄더 4쌍 | T1 명령 + `--stage NONE` | exact % 가 T1 보다 20 pp 이상 하락(removeStage 동작 확인), `params_effective.stage == NONE` |
| T3 | T1 6쌍 | (T1 결과 재사용) + 파이썬 레퍼런스 `simulate.py` 의 `remove_stage`→`process_high_curvature` 를 같은 patch/overlap 으로 실행 | exe 결과 vs 파이썬 결과 ±2 이내 ≥ 96% (파이썬 vs 현장 수치도 참고용으로 기록) |
| T4 | tire2 폴더 | `batch --folder tire2 --out d --fovproc <FOVPROC.ini> --ini <ini> --dump min` 후 6쌍 개별 `run` | exit 0, `batch.json` 항목 8개, `result.png`+`stats.json` 을 가진 `<imgIdx>_<name>` 폴더 8개, 각 항목 교차검증·파라미터 일치, 개별 `run` 결과가 batch 결과와 화소 단위로 동일. 각 항목과 개별 run 은 T1 과 같은 임계(exact ≥ `exact_min_pct` 99.9, 검정 IoU ≥ 0.99)를 넘지 못하면 FAIL |
| T5 | — | `run --in <없는 파일>`, `run … --patch 0,15`, `run … --overlap 1.5` | 각각 exit 3(+stderr 한 줄 JSON `{"error":…}`), exit 2, exit 2 |

기준 소스가 현장 PC3 소스(2026-07-08)로 바뀐 뒤에는 exact 100 % 가 정상이며, `exact_min_pct`(99.9) 미달은 회귀로 본다. 임계값은 모두 `cases.json` 의 `expect` 에서 조정한다.

### 3.2 측정 지표 정의

result 는 exe 의 `result.png`(8U), ref 는 현장 `_Proc.mim`(float32 0..255 → 반올림 후 uint8) 이다.

- `exact_pct`: result == ref 인 화소 비율. `within1_pct`/`within2_pct`: |result−ref| ≤ 1 / ≤ 2 인 비율. `max_abs`: 최대 |result−ref|. `mismatch_count`: 다른 화소 수.
- `black_mask_iou`: (result==0)∩(ref==0) / (result==0)∪(ref==0). 0 은 null 과 하위 클립 두 의미를 가진다(DLL 과 동일).
- `black_agree_pct`: 두 영상의 검정(0) 여부가 같은 화소 비율(파이썬 레퍼런스 메모의 "검정 마스크 99.6%" 와 같은 정의).
- `ref_black_not_result` / `result_black_not_ref`: ref 만 검정 / result 만 검정인 화소 수.
- 교차검증 허용치: 비율(%) 항목 ±0.01 pp, IoU ±0.0001, 화소 수 항목 전체 화소의 0.01%, `max_abs` 는 완전 일치.

### 3.3 산출물

- `tests/results/RESULTS.md`: 요약 표 + 케이스별 표(각 행에 수치·교차검증·파라미터·판정), 실패 행의 사유 목록.
- `tests/results/results.json`: 위 전부와 실행 명령, exe 종료 코드, stderr 꼬리, `stats.json` 의 `ref`/`null`/`clip` 블록.
- `tests/results/T*/…`: exe 가 생성한 파일 그대로(재실행 시 해당 케이스 폴더만 삭제 후 재생성).

## 4. ui_smoke.py 실행 (T6)

```
D:\anaconda\python.exe tests\ui_smoke.py --exe build\Release\depth_sim.exe --port 8799
D:\anaconda\python.exe tests\ui_smoke.py --exe build\Release\depth_sim.exe --skip-batch   # 배치 생략(빠른 확인)
```

동작: `node ui/server.js --port 8799 --cli <exe> --ini <레시피 ini>` 기동(로그 `tests/results/ui_smoke_server.log`) → 포트 대기(30 s) →
`GET /api/config`(키 cli/roots/presets/mapping, 프리셋 3개 이상) → `POST /api/run`(tire1 7→12, 프리셋 INSHOULDER 파라미터, dump all;
키 runId/outDir/stats/files, `stats.ref` 존재) → `GET /runs/<id>/result.png`(200, PNG) → `GET /api/pixel?runId&x=4000&y=1200`
(키 raw/basis/diff/result/ref, raw·result 값 존재) → `POST /api/batch`(tire2, fovproc; 키 batchId/items, 항목 8개, 각 항목 imgIdx/name/stats/files;
타임아웃 기본 1200 s) → 서버 종료. 결과는 `tests/results/ui_smoke.json`, 종료 코드 0/1.
옵션: `--node <node.exe>`, `--server <server.js>`, `--no-ini`, `--batch-timeout N`, `--start-timeout N`, `--out <dir>`.

## 5. fake_exe.py (파이썬 스탠드인) — 하네스 개발용

`tests/fake_exe.py` 는 `depth_sim.exe` 와 같은 CLI 계약(`run`/`batch`/`info`/`version`, 종료 코드 0/2/3/4, stderr 한 줄 JSON,
`result.png`+`stats.json`(`ref` 블록), `batch.json`, `<imgIdx>_<name>` 폴더)을 `simulate.py` 위에 흉내 낸 것으로, C++ 빌드가 없을 때
하네스(`verify.py`) 자체를 점검하는 용도다. DLL 과 비트 동일하지 않으므로 **검증 수치의 근거로 쓰지 않는다**.

```
D:\anaconda\python.exe tests\verify.py --exe tests\fake_exe.py --out tests\results_fake
```

**최종 검증 수치는 `tests/results/RESULTS.md`(C++ exe, 현장 PC3 소스 기준) 를 본다.** 2026-09-18 결과: T1 정상 표본 6장·T4 이상 표본 8장 모두
현장 `_Proc` 와 exact 100.000 %(불일치 0 px, 검정 IoU 1.0), T2 `--stage NONE` 하락 49~56 pp, T3 파이썬 레퍼런스 대비 99.98~99.99 %,
T5 exit 2/3 규약 확인, T6 UI 스모크 12/12 PASS. 임계값은 `cases.json` 의 `expect`(T1/T4 exact ≥ 99.9, 검정 IoU ≥ 0.99, T2 하락 ≥ 20 pp,
T3 ±2 이내 ≥ 96 %) 에 있고, T4 는 항목·개별 run 모두 T1 과 같은 임계로 판정한다.

## 6. 주의 사항

- H: 데이터 폴더 이름에 한글이 포함된다. 하네스는 `tifffile`/`np.fromfile`+`cv2.imdecode` 로 읽어 문제가 없고, exe 에는 subprocess 리스트 인자(UTF-16 명령줄)로 그대로 전달된다.
  exe 쪽은 `wmain` 또는 UTF-8→wide 변환으로 열어야 한다. 하네스가 만드는 파일은 모두 ASCII 경로(`tests/results`)에 둔다.
- `tests/results/T*` 는 케이스 실행 시마다 삭제·재생성된다. 보관하려면 `--out` 을 다른 폴더로 지정한다.
- 콘솔 코드페이지가 UTF-8 이 아니면 로그의 한글이 깨져 보일 수 있으나 `results.json`/`RESULTS.md` 는 UTF-8 로 정상 기록된다.
