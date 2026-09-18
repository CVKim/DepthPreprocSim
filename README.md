# DepthPreprocSim

`alg_depth_preproc.dll`(talos-platform, PREPROC_3D, Gocator 3D depth 전처리)의 오프라인 시뮬레이터입니다.
MIL/talos 없이 `.mim` 파일을 직접 읽어 DLL 과 **바이트 단위로 동일한** 결과를 재현하고, 파라미터를 바꾸며 중간 단계를
시각화합니다. 계약 문서는 `DESIGN.md` 입니다.

## 1. 빌드

### 요구 사항
- Windows 11 x64, Visual Studio 의 **v142 툴셋(MSVC 14.29.30133)**
  - VS 18 Insiders 에 v142 가 함께 설치되어 있으면 그대로 사용합니다(`-G "Visual Studio 18 2026" -A x64 -T v142`).
  - 대안: VS 2019(`-G "Visual Studio 16 2019" -A x64`).
- CMake 3.20 이상(VS 번들 cmake 4.3.1 또는 시스템 cmake 4.0.1).
- OpenCV 4.4.0 사전 빌드: `C:\Program Files\AIV\ThirdParty\opencv\opencv_440`
  (`include\`, `lib\x64_Release\opencv_world440.lib`, `bin\x64_Release\opencv_world440.dll`, Debug 는 `x64_Debug\opencv_world440d.*`).
  다른 위치이면 `-DOPENCV_ROOT=<경로>` 로 지정합니다.

DLL 과 같은 OpenCV 바이너리·같은 툴셋(v142)·같은 부동소수점 모델(`/fp:precise`)·같은 최적화(Release `/O2 /Oi /GL` + `/LTCG`)를
사용해야 비트 동일성이 보장됩니다.

### 빌드 방법
```bat
cd E:\Dev\DepthPreprocSim
build.bat            rem Release (기본)
build.bat Debug      rem Debug: 캡처 파이프라인 == 원본 함수 결과 assert 가 활성화됩니다
```
`build.bat` 은 VS 번들 cmake(VS 18 2026 + v142)를 먼저 시도하고, 실패하면 시스템 cmake(VS 16 2019)로 재구성합니다.
산출물은 `build\Release\depth_sim.exe` 이며 `opencv_world440.dll` 이 자동으로 옆에 복사됩니다.

수동 구성 예:
```bat
"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S . -B build -G "Visual Studio 18 2026" -A x64 -T v142
cmake --build build --config Release
```

### 소스 구성
| 경로 | 내용 |
|---|---|
| `core/DepthPreprocCore.h/.cpp` | `namespace DepthProcessor` 7개 함수를 **현장(PC3) 빌드 소스**(`reference/site_pc3_20260708/3dDepthProcessing.cpp`, 2026-07-08, rc 1.0.2.0.38cf930_HT)에서 **그대로 복사**. `SimPipeline::Run` 이 `C3DPreprocess::INSPECT` 흐름(ROI 뷰 → 타입별 지그 제거 → processHighCurvature → ROI 복원)을 재현하고 중간 단계를 캡처. 실험 경로는 `DepthProcessorExp` 로 분리 |
| `reference/site_pc3_20260708/` | 현장 PC3 에서 실제로 돌아가는 `alg_depth_preproc` 소스 사본(cpp/h/rc). talos-platform 커밋 0a6814f2 와는 지그 제거 단계만 다름(§3.3) |
| `core/MimReader.h/.cpp` | MIL `.mim`(little-endian TIFF, 무압축, 단일 채널, float32/uint8/uint16, 다중 strip) 읽기, float32 단일 strip TIFF 쓰기. 그 외 확장자는 `cv::imdecode(IMREAD_UNCHANGED)` |
| `core/IniReader.h/.cpp` | `alg_depth_preproc.ini` `[CALxxxx]`, `FOVPROC.ini` `[FOVPROCxxxx]` 파서(DLL `ReadSection` 과 같은 규칙) |
| `core/Stats.h/.cpp` | 통계·참조 비교·히스토그램·`stats.json` 직렬화(외부 라이브러리 없음) |
| `cli/main.cpp` | `depth_sim.exe` (wmain, 한글 경로 지원) |

## 2. CLI 사용법 (`build\Release\depth_sim.exe`)
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
depth_sim.exe info --in "H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060\7_SWU_Sh_R_D.mim"
depth_sim.exe run --in "...\7_SWU_Sh_R_D.mim" --ref "...\12_SWU_Sh_R_D_Proc.mim" --out runs\smoke7 --patch 15,15 --overlap 0.25 --stage AUTO
depth_sim.exe batch --folder "H:\000. PJT\01. HankookTire\03_이미지\PC3\2000026091715281105" --out runs\tire2
```
`run` 은 `<out>` 아래에 `result.png`, `result_f32.tif`, `raw_vis.png`, `null_mask.png`, `stage_removed_mask.png`, `scaled.png`,
`basis.png`, `diff.png`, `clipped.png`, (`--ref` 시) `ref.png`, `ref_diff.png`, 각 `preview_*.png`, `raw.f32 basis.f32 diff.f32`,
`stats.json` 을 만들고 `stats.json` 내용을 stdout 에 출력합니다. 종료 코드: 0 성공, 2 인자 오류, 3 입력 읽기 실패, 4 처리 예외.
오류는 stderr 에 한 줄 JSON `{"error":"..."}` 으로 출력합니다.

### 주의 사항
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

## 3. 검증 결과 (2026-09-18, Release 빌드, v142 14.29.30133, OpenCV 4.4.0, 기준 = 현장 PC3 소스 2026-07-08)

`python tests\verify.py --exe build\Release\depth_sim.exe --cases tests\cases.json --out tests\results` 결과(`tests\results\RESULTS.md`).
전체 판정 **PASS**(T1~T5), UI 스모크 12/12 PASS.

### 3.1 현장 `_Proc` 대비 — 14장 모두 100 % 비트 동일
| 케이스 | 표본 | 이미지 | exact % | 불일치 px | 검정 IoU |
|---|---|---|---|---|---|
| T1 | 정상 표본 `1639390184_2000026091709423060` | 숄더 5→11, 7→12, 17→23, 19→24 / 비드 3→10 / 센터 1→9 | **100.000** ×6 | 0 | 1.0000 |
| T4 (batch, `--fovproc --ini`) | 이상 표본 `2000026091715281105` | 1, 3, 5, 7, 13, 15, 17, 19 → 9, 10, 11, 12, 21, 22, 23, 24 | **100.000** ×8 | 0 | 1.0000 |

exe 의 `stats.json` `ref` 블록과 `verify.py` 의 독립 계산이 0.01 % 이내로 일치(교차검증)하고, `batch` 결과와 개별 `run` 결과도 동일합니다.
처리 시간은 현장 로그(9/14 PC3, 58회 중앙값)와 같은 수준입니다: 숄더 1.35 s(현장 1.32), 센터 2.95 s(3.00), 비드 0.56 s(0.62).

### 3.2 지그 제거 확인 (T2)
같은 숄더 입력을 `--stage NONE` 으로 돌리면 exact 가 100 → 43.9~50.6 % 로 떨어집니다(하락 49~56 pp). 현장 DLL 에 지그 제거가 들어 있고
시뮬레이터가 같은 규칙을 쓰는 것을 뜻합니다.

### 3.3 현장 소스와 talos-platform 소스의 차이, 그리고 현장 코드의 실제 동작
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

### 3.4 파이썬 레퍼런스 대비 (T3)
`tests/reference/simulate.py`(numpy/cv2 독립 구현, 타입별 실효 규칙 포함) 와 exe 결과는 6쌍 모두 exact 99.979~99.994 %, ±1 이내 100 %,
검정 IoU 1.0000 입니다(남는 차이는 float32/double 반올림).

### 3.5 CLI 계약 확인 (T5)
없는 입력 → exit 3 + stderr JSON, `--patch 0,15` / `--overlap 1.5` → exit 2. 이미지를 벗어나는 ROI → exit 4(DLL 과 같은 실패 지점).
`--ini ... --cal N` 으로 CAL 의 PatchSize/Overlap/DepthPreprocType 이 로드되고, `--type` 이 지그 제거 규칙을 바꿉니다
(`--type BEAD` 나 `--break-kernel 0` 으로 숄더를 돌리면 100 % 가 깨지고 `dll_identical` 이 false 가 됩니다).

### 3.6 성능 참고
처리 자체는 숄더 1장 약 1.35 s, 센터 약 2.95 s 입니다. `--dump all` 산출물(약 250 MB)을 E: 에 쓰면 수십 초가 더 걸리므로 빠르게 반복할 때는
`--dump min` 또는 빠른 디스크의 `--out` 을 사용하십시오. 상대 `--out` 은 `Result\runs\` 아래로 해석됩니다(절대 경로 권장).

## 4. UI (`ui/server.js` + `ui/public`)

브라우저에서 입력 이미지를 고르고 파라미터를 바꿔 `depth_sim.exe` 를 실행한 뒤, 중간 단계 이미지·통계·참조 비교를 한 화면에서
확인하는 로컬 웹 UI 입니다. Node 내장 모듈(http, fs, path, child_process, zlib)만 사용하므로 `npm install` 이 필요 없고,
외부 CDN·폰트를 쓰지 않아 오프라인에서 동작합니다. 모든 데이터는 로컬 PC 안에서만 오갑니다(기본 바인딩 127.0.0.1).

### 4.1 시작
```bat
cd E:\Dev\DepthPreprocSim
"C:\Program Files\nodejs\node.exe" ui\server.js --port 8765
```
브라우저에서 `http://localhost:8765` 를 엽니다. 서버 옵션(모두 선택):

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--port N` | 8765 | HTTP 포트 |
| `--host A` | 127.0.0.1 | 바인딩 주소. 다른 PC 에서 접속하려면 `0.0.0.0` |
| `--cli <exe>` | `build\Release\depth_sim.exe` | 실행할 CLI. `node ui\fake_cli.js` 또는 `.js` 경로를 주면 Node 로 실행합니다(개발용) |
| `--root <dir>` | `H:\000. PJT\01. HankookTire\03_이미지`, `D:\AIV\MODEL`, `E:\` | 폴더 탐색기의 시작 루트. 여러 번 지정하거나 `;` 로 구분합니다 |
| `--ini <file>` | `D:\AIV\MODEL\[1]TireInspect_PC3_DEPLOY\alg_depth_preproc.ini` | 프리셋(`[CAL0001..3]`) 원본. 없으면 레시피 기본 3종을 내장값으로 사용합니다 |
| `--fovproc <file>` | `D:\AIV\MODEL\[1]TireInspect_PC3_DEPLOY\FOVPROC.ini` | `RequireImgIdx → ResultImgIdx / ParamIdx` 매핑. 없으면 1→9, 3→10, 5→11, 7→12, 13→21, 15→22, 17→23, 19→24 를 사용합니다 |
| `--runs <dir>` | `Result\runs\ui\` | 실행 결과 폴더(업로드는 `Result\uploads\`). E: 가 느릴 때 빠른 디스크로 옮길 수 있습니다 |
| `--uploads <dir>` | `uploads\` | 드래그앤드롭 업로드 저장 폴더 |

실행 결과는 `runs\<runId>\` (`runId` = `YYYYMMDD_HHMMSS_NNN`, 배치는 `batch_` 접두)에 CLI 산출물 그대로 저장되고,
서버가 `run.json`(요청 파라미터·명령줄) 과 `cli_stderr.txt` 를 함께 남깁니다. `--dump all` 은 원본 크기 float32 덤프 3장을
포함하므로 숄더(8192×1940) 1회 ≈ 190 MB, 센터(12160×1940) 1회 ≈ 280 MB, 배치 1회 ≈ 1.5 GB 를 차지합니다.
필요 없는 실행 폴더는 그냥 삭제하면 됩니다(서버 재시작 불필요).

### 4.2 화면 구성 — 단일 실행 탭
**상단 바**: 탭(단일 실행 / 배치), CLI 상태 배지(경로가 없으면 빨간색), 실험 옵션 경고 배지
(`stats.params_effective.dll_identical=false` 일 때 “실험 옵션 사용 중 — DLL 과 결과가 다를 수 있습니다”).

**좌측 패널**
- **입력 파일**: 경로를 직접 입력하거나 [탐색] 으로 서버 폴더를 탐색합니다(폴더 클릭 = 이동, 파일 클릭 = 선택, `_Proc` 파일은 주황색).
  점선 영역에 파일을 끌어다 놓으면 `PUT /api/upload` 로 `uploads\` 에 저장되고 그 경로가 입력란에 들어갑니다.
  이름이 `<idx>_…_D.mim` 이면 FOVPROC 매핑으로 결과 인덱스와 CAL 번호를 표시하고 프리셋을 자동 선택합니다.
- **참조(_Proc) 파일**: 입력 이름의 인덱스를 매핑(예: 7 → 12)해 같은 폴더의 `12_…_D_Proc.mim` 이 있으면 자동으로 채웁니다.
  직접 입력하면 자동 추정을 멈추고, [추정] 을 누르면 다시 추정합니다. 비워 두면 참조 비교 없이 실행합니다.
- **파라미터**: `alg_depth_preproc.ini` 의 `[CALxxxx]` 프리셋을 고르면 아래 값이 채워지고, 값을 손대면 “(사용자 지정)” 으로 바뀝니다.
  - `DepthPreprocType` — 표시용입니다(INNERCENTER/BEAD/INSHOULDER 모두 `processHighCurvature` 동일 경로).
  - `PatchSize W / H` — `cv::Size(W,H)`, 첫 값이 가로 픽셀입니다. 기준면(basis)을 만드는 패치 median 의 창 크기입니다.
  - `Overlap` — (0,1) 개구간. `step = int(patch × (1 − overlap))` (최소 1) 이 자동 표시됩니다. 레시피 기본 0.25(CAL1 은 0.1).
  - `StagePosition` — AUTO/TOP/BOTTOM 은 모두 같은 동작(최대 유효 성분만 남기고 나머지를 null 로), NONE 은 지그 제거 생략.
  - `ROI x1,y1,x2,y2` — `x2>x1 && y2>y1` 일 때만 사용하며 그 밖(예: 9999,9999,0,0)은 전체 이미지 처리. 사용 시 결과 밖은 0 이고 패널에 노란 사각형으로 표시됩니다.
  - `Lower / Upper Percentage` — DLL 은 읽기만 하고 5/95 를 고정 사용합니다. 아래 “ini 백분위 사용” 을 켠 경우에만 적용됩니다.
- **실험 옵션**(하나라도 켜면 DLL 과 결과가 달라지고 경고 배지가 뜹니다): ini 백분위 사용(`exp_use_ini_pct`),
  null 제외 유효 픽셀로 백분위 계산(`exp_valid_pct`), 패치 median 에서 null 제외(`exp_masked_median`),
  null 출력값(`exp_null_value`, −1 = DLL 처럼 0), 구멍 채움 최대 px(`exp_fill_holes`, 0 = 끄기).
- **출력 옵션**: 덤프 `all`(중간 단계 PNG + `raw/basis/diff.f32`) / `min`(`result.png` + `stats.json` 만; 이 경우 중간 레이어와
  픽셀 인스펙터의 raw/basis/diff 는 표시되지 않습니다), 미리보기 축소 배율(기본 8), 화소 크기 um(비우면 CLI 기본 300/100).
- **[실행]** — CLI `run` 을 호출합니다. 실행 중에는 경과 시간이 표시되고 버튼이 잠깁니다. 실패하면 CLI 의 stderr JSON 메시지를 그대로 보여줍니다.
- **[A/B 고정]** — 현재 결과를 A 로 고정합니다. 이후 실행 결과는 B 로 들어오며 **[A/B 전환]** 또는 `Space` 키로 두 결과를 같은 줌·팬 상태에서 번갈아 봅니다.
  다시 누르면 고정을 해제합니다.
- **이전 실행 불러오기** — `runs\` 의 과거 실행(배치 포함)을 다시 열어 봅니다(서버 재시작 후에도 가능).

**우측 패널**
- **레이어 체크박스** → 패널 그리드. 해당 실행에 산출물이 없는 레이어(덤프 min, 참조 없음)는 비활성화됩니다.
  | 레이어 | 파일 | 의미 |
  |---|---|---|
  | raw (null=빨강) | `raw_vis.png` | 원본 z 를 1~99 백분위로 8U 정규화, null(−999) 은 빨강. 파란 점선 = 유효 행 범위(띠) |
  | null 마스크 | `null_mask.png` | 원본 null 픽셀 = 255 |
  | 지그 제거 마스크 | `stage_removed_mask.png` | `removeStageFromRawData` 가 −999 로 바꾼 픽셀 = 255 |
  | scaled (16U) | `scaled.png` | `scaleTo16bitIgnoreNull` 결과(0..65535). 브라우저는 16-bit PNG 의 상위 8비트로 표시합니다 |
  | 기준면 basis | `basis.png` | 패치 median + 선형 리사이즈로 만든 기준면(16U) |
  | diff | `diff.png` | `scaled − basis` 를 [−1000, 1000] → 0..255 로 선형 매핑(128 = 0) |
  | clipped | `clipped.png` | `postClipNormalize` 의 클립 후 값을 [low, high] → 0..255 |
  | 결과 result | `result.png` | DLL 출력과 동일한 8U 결과 |
  | 참조 ref | `ref.png` | `--ref` 로 준 현장 `_Proc` 8U |
  | \|result − ref\| × 8 | `ref_diff.png` | 불일치 강조(차이 1 = 8, 32 이상은 255) |
- **동기 줌·팬**: 휠 = 커서 기준 확대/축소, 드래그 = 이동, 더블클릭 또는 [보기 초기화]/`0` 키 = 전체 보기. 모든 패널이 같은 뷰포트를 공유합니다.
  축소 상태에서는 `preview_*.png` 를, 확대해 미리보기 해상도를 넘으면 원본 PNG 를 자동으로 불러옵니다(패널 우측 상단에 표시).
  원본 픽셀보다 크게 확대하면 보간 없이 픽셀 단위로 그립니다.
- **픽셀 인스펙터**: 마우스 위치의 `(x, y)`, raw z(null 이면 빨간 “null”), basis, diff, result, ref(`Δ = result − ref`) 를
  `GET /api/pixel` 로 조회합니다(60 ms 디바운스). 서버는 `raw/basis/diff.f32` 를 오프셋으로 직접 읽고, result/ref 는
  내장 PNG 디코더(8/16-bit 그레이·비인터레이스)로 읽습니다. 덤프가 없으면 해당 값은 “–” 입니다. 모든 패널에 십자 마커가 함께 표시됩니다.
- **통계 표**(`stats.json`): 입력 크기, 적용 파라미터(step 포함), null 개수·비율, 지그 제거 개수·비율, 유효 성분 수, 유효 행 범위,
  z 최소/최대와 scaled 1 단위의 mm 환산, low/high(scaled 단위와 mm), 정규화 min/max, diff==0 비율, 출력 검정/흰색 비율,
  참조 비교(정확 일치·±1·±2·최대 차이·검정 마스크 IoU·한쪽만 검정인 픽셀 수), 단계별 처리 시간.
  **결과의 0 은 “null” 과 “하위 클립(low 이하)” 두 의미가 있습니다**(DLL 과 동일). 통계는 이를 분리합니다:
  `검정(0) 전체` 는 둘을 합한 값이고, `유효 내 검정` 은 null 을 제외한 하위 클립만, `원본 null`/`지그 제거` 는 null 쪽입니다.
- **diff 히스토그램**: 유효 픽셀의 diff 분포([−1000, 1000], 256 구간). 파란 선 = low, 주황 선 = high, 회색 점선 = 0. 기본 로그 스케일.
- **행 구간별 검정/흰색 비율**: 유효 행(띠)을 20 구간으로 나눠 각 구간의 유효 픽셀 대비 0/255 비율을 막대로 그립니다.
  가장자리 구간의 검정 비율이 높으면 띠 경계에서 하위 클립이 몰린다는 뜻입니다.

### 4.3 배치 탭
타이어 폴더(예: `…\PC3\2000026091715281105`)와 `FOVPROC.ini` 를 지정하고 [배치 실행] 을 누르면 CLI `batch` 가 폴더의 `*_D.mim`
(`_Proc` 제외)을 매핑대로 처리하고 같은 폴더의 `_Proc` 가 있으면 자동 비교합니다. 이미지별 파라미터는 `ParamIdx → [CALxxxx]`
(서버의 `--ini`)를 따르며, “좌측 파라미터를 모든 이미지에 강제 적용” 을 켜면 패치/오버랩/스테이지/ROI 를 모든 이미지에 덮어씁니다.
실험 옵션·덤프·미리보기 설정은 항상 좌측 값을 따릅니다. 결과는 8행 표(인덱스, 이름, 결과·참조 미리보기, 정확 일치 %, ±1 %,
검정 IoU, null %, 유효 내 검정 %, 처리 ms)로 나오고, 행을 클릭하면 그 이미지를 단일 실행 탭에서 열어 레이어·픽셀 인스펙터로 살펴볼 수 있습니다.

### 4.4 API 요약 (`DESIGN.md` §4)
| 요청 | 응답 |
|---|---|
| `GET /api/config` | `{cli, cliExists, roots[], presets[{name,cal,type,patch,overlap,lower,upper,stage,roi}], mapping{idx:{result,cal,name}}, ini, fovproc, runsDir}` |
| `GET /api/browse?path=` | `{path, parent, dirs[], files[{name,size,kind}]}` (.mim/.tif/.png/.bmp/.jpg/.ini). `path` 를 비우면 루트 목록 |
| `POST /api/run` `{input, ref, params}` | `{runId, outDir, stats, files{result:"/runs/<id>/result.png",…}, cli{durationMs,cmd}}`. CLI 종료 코드 2→400, 3→404, 4→500 이며 본문에 stderr JSON 의 `error` |
| `POST /api/batch` `{folder, params, fovproc}` | `{batchId, items[{imgIdx,name,runId,stats,files}], batch}` |
| `GET /api/pixel?runId&x&y` | `{raw, basis, diff, result, ref, is_null}` (덤프 없으면 null) |
| `PUT /api/upload?name=` (raw body) | `{path, size}` |
| `GET /runs/<id>/<file>` | 산출물 정적 제공 |
| `GET /api/runs`, `GET /api/run?runId=` | (추가) 과거 실행 목록 / 재로드 |

`params` 는 `{type, patch_w, patch_h, overlap, lower, upper, stage, roi:[x1,y1,x2,y2]|null, exp:{use_ini_pct, valid_pct, masked_median, null_value, fill_holes}, dump, preview, px_x, px_y}` 이며
`patch:"15,15"` / `patch:[15,15]` 형태도 받습니다. 서버는 CLI 를 `child_process.spawn`(shell 없음)으로 실행하고 한 번에 하나씩 순서대로 처리합니다.

### 4.5 개발용 가짜 CLI 와 스모크 테스트
`ui\fake_cli.js` 는 `depth_sim.exe` 와 같은 명령·옵션·종료 코드·산출물 구성을 흉내 내지만 **합성 데이터**를 만드는 개발용 대체물입니다
(결과는 DLL 과 무관하며 `tool_version` 에 FAKE 로 표기). C++ 빌드 없이 UI 를 개발·점검할 때 사용합니다.
```bat
"C:\Program Files\nodejs\node.exe" ui\server.js --port 8765 --cli "node ui\fake_cli.js"
"C:\Program Files\nodejs\node.exe" ui\smoke_test.js --cli "node ui\fake_cli.js"              rem 가짜 CLI 로 전 API 점검
"C:\Program Files\nodejs\node.exe" ui\smoke_test.js --cli build\Release\depth_sim.exe        rem 실제 CLI 로 T6 점검
```
`ui\smoke_test.js` 는 서버를 띄워 `/`, `/api/config`, `/api/browse`, `/api/run`(tire1 7→12), `/runs/...`, `/api/pixel`, `/api/upload`,
`/api/batch`(tire2), 오류 경로(없는 파일 → 404, 잘못된 patch → 400)를 확인한 뒤 서버를 종료합니다.

## 검증 로그
2026-09-18 (TASK A, C++ core/CLI)
```
build.bat Release                      -> build\Release\depth_sim.exe (MSVC 19.29.30159 = v142, /O2 /Oi /GL /LTCG, opencv_world440.dll 복사, MD5 원본과 동일)
cmake --build build --config Debug     -> build\Debug\depth_sim.exe (assert 활성) 정상 실행, result.png MD5 = Release
depth_sim info --in ...\7_SWU_Sh_R_D.mim         8192x1940 float32, null 5321004 (33.48%), valid -53.1895 / 18.6545
depth_sim run  7_SWU_Sh_R_D --ref 12_..._Proc --patch 15,15 --overlap 0.25 --stage AUTO   -> runs\smoke7
   ref.exact_pct 65.12  within1 90.31  within2 98.08  black_mask_iou 0.98780  capture_identical true  timing total 1317 ms
depth_sim run  ... --stage NONE                  -> exact 43.59  within2 54.30  iou 0.6767 (T2: 크게 나빠짐 확인)
depth_sim batch --folder ...\2000026091715281105 --out runs\tire2_min --dump min   -> exit 0, 8장, BEAD 2장 exact 100.00 / IoU 1.0
depth_sim batch ... --fovproc FOVPROC.ini --ini alg_depth_preproc.ini             -> exit 0, 매핑 1→9,3→10,5→11,7→12,13→21,15→22,17→23,19→24 / CAL 1,2,3
오류 경로: 없는 파일 exit 3 / --patch 15x15 exit 2 / --overlap 1.5 exit 2 / --out 누락 exit 2 / 이미지 밖 ROI exit 4
```
