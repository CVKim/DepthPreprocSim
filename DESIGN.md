# DepthPreprocSim — alg_depth_preproc.dll 오프라인 시뮬레이터 설계 계약

작성 2026-09-18. 목적: talos-platform `alg_depth_preproc.dll`(Gocator 3D depth 전처리, PREPROC_3D) 과
**비트 동일한 결과**를 MIL/talos 없이 .mim 파일에서 직접 재현하고, 파라미터를 바꾸며 중간 단계를 시각화하는 도구.

## 0. 절대 원칙
1. 기본 파라미터(실험 옵션 전부 off)에서의 출력은 DLL 과 **바이트 단위로 동일**해야 한다.
   - **기준 소스는 현장(PC3) 빌드 소스**다: `H:\000. PJT\01. HankookTire\04_코드\PC3_Platform\alg_depth_preproc\3dDepthProcessing.cpp`
     (2026-07-08 16:55, rc 1.0.2.0.38cf930_HT; 사본 `reference/site_pc3_20260708/`). talos-platform 커밋 0a6814f2(feat/ALG-288) 와는
     지그 제거 단계만 다르다(현장: `removeStageFromRawData(src, stage, breakKernel)` 침식→최대 성분→팽창∧마스크, 기존 함수는
     `removeStageFromRawDataBead` 로 유지, 디버그 MbufSave 주석 처리).
   - `namespace DepthProcessor` 의 실행 경로 함수(`removeStageFromRawData`, `removeStageFromRawDataBead`, `computePercentile`,
     `postClipNormalize`, `scaleTo16bitIgnoreNull`, `patchBasedMedian`, `processHighCurvature`) 는 현장 소스에서 **그대로 복사**한다
     (공백·주석 외 변경 금지). 파일 상단에 원본 경로·날짜·rc 버전을 주석으로 남긴다.
   - `C3DPreprocess::INSPECT` 의 흐름(ROI 처리 → 타입별 지그 제거 → processHighCurvature → ROI 복원)도 동일하게 재현한다.
     타입별 지그 제거: INNERCENTER 없음, BEAD `removeStageFromRawDataBead(src, stage)`, INSHOULDER `removeStageFromRawData(src, stage, 3)`.
   - **현장 코드의 실제 동작(주의)**: `removeStageFromRawData` 안의 `cv::Mat labelMask = mask;` 는 얕은 복사이고
     `cv::erode(mask, labelMask, kernel)` 이 제자리(in-place)로 실행되어 `mask` 자체가 침식된다. 따라서 마지막
     `bitwise_and(keep, mask)` 는 침식된 마스크와 교집합이 되어 "팽창으로 경계 복원" 은 실제로 일어나지 않는다.
     실효 규칙 = **침식된 최대 성분만 유지**(모든 경계·구멍 가장자리 1 px 추가 제거). 시뮬레이터는 현장 소스를 그대로
     복사하므로 같은 동작을 하며(현장 출력과 100 % 일치, 2026-09-18), 이 동작을 바꾸는 것은 현장 코드 수정 사항이다.
   - 같은 OpenCV 바이너리를 링크한다: `C:\Program Files\AIV\ThirdParty\opencv\opencv_440`
     (include, `lib\x64_Release\opencv_world440.lib`, `bin\x64_Release\opencv_world440.dll`; Debug 는 `x64_Debug\opencv_world440d.*`).
   - 툴셋 v142(MSVC 14.29), C++17, Release /O2, /fp:precise (DLL vcxproj 와 동일).
2. 실험 옵션(§3 `exp_*`)은 **명시적으로 켠 경우에만** 동작하며, 기본 경로 코드에 분기를 섞지 않는다
   (기본 경로 = 원본 함수 호출, 실험 경로 = 별도 함수).
3. 파라미터 의미는 DLL 의 `ReadSection` 과 같다:
   - `PatchSize = W,H` → `cv::Size(W,H)` (첫 값이 width=가로 px, 둘째가 height). 기본 15,15.
   - `Overlap` (0,1) 개구간. 없으면 0.5. step = int(patch*(1-overlap)), 최소 1.
   - `Lower/Upper Percentage`: DLL 은 읽기만 하고 **5/95 하드코딩**. 시뮬레이터 기본 경로도 5/95 고정.
     `exp_use_ini_pct=1` 일 때만 지정값 사용(이 경우 결과가 DLL 과 달라진다고 표시).
   - `StagePosition`: AUTO(0, 기본)·TOP(1)·BOTTOM(2)·NONE(3). DLL 은 NONE 만 생략, 나머지는 모두 AUTO 와 동일 동작.
   - `ROI = x1,y1,x2,y2`; `x2>x1 && y2>y1` 일 때만 사용. 레시피 기본 `9999,9999,0,0` = 미사용.
   - `DepthPreprocType` INNERCENTER/BEAD/INSHOULDER 는 `processHighCurvature` 는 공통이지만 **지그 제거 단계가 다르다**(위 참조).
     `--break-kernel`(기본 3 = 현장 리터럴) 을 3 이외로 주면 INSHOULDER 결과가 DLL 과 달라지므로 `dll_identical=false` 로 표기한다.
4. 입력은 32-bit float 단일 채널(null = -999.f). 8/16-bit 입력은 DLL 처럼 float 으로 변환만 한다.
5. 출력 8U 결과의 0 은 null 또는 하위 클립 두 의미가 있다(DLL 과 동일). 시뮬레이터는 통계로 둘을 분리해 보여준다.

## 1. 폴더 구조
```
E:\Dev\DepthPreprocSim\
  DESIGN.md                 이 문서
  README.md                 빌드·실행·UI 사용법(한국어)
  CMakeLists.txt            core(static) + depth_sim(exe) + tests
  build.bat                 CMake(VS 번들 cmake 우선) 구성·빌드; 실패 시 vcvars+ninja/nmake 대안 주석
  core/
    DepthPreprocCore.h/.cpp 원본 DepthProcessor 함수(그대로) + SimPipeline(INSPECT 재현, 중간단계 캡처) + 실험 경로
    MimReader.h/.cpp        MIL .mim(TIFF little-endian) 읽기/쓰기: tags 256,257,258,259(=1 무압축만),262,273,277(=1),278,279,339
                            float32/uint8/uint16 단일 채널, 단일/다중 strip. .tif/.png/.bmp 는 cv::imread(IMREAD_UNCHANGED) 폴백.
                            쓰기: float32 단일 strip TIFF(.tif/.mim) — MIL 로 다시 읽히는지는 미보장(README 에 명시).
    IniReader.h/.cpp        `alg_depth_preproc.ini` [CALxxxx] 섹션(키: Name, DepthPreprocType, ROI, PatchSize, Lower Percentage,
                            Upper Percentage, Overlap, StagePosition) + `FOVPROC.ini` [FOVPROCxxxx](RequireImgIdx, ResultImgIdx, ParamIdx)
    Stats.h/.cpp            통계·비교·히스토그램·JSON 직렬화(외부 json 라이브러리 없이 직접 작성)
  cli/main.cpp              depth_sim.exe (§2)
  ui/server.js              Node 내장 http 만 사용(npm 의존성 0). CLI 실행·정적 파일·API
  ui/public/index.html, app.js, style.css   단일 페이지 UI (§4)
  tests/verify.py           산출물 vs 현장 _Proc / 파이썬 레퍼런스 비교 (numpy, tifffile, cv2 사용 가능)
  tests/cases.json          검증 케이스 목록(§5)
  runs/                     UI 실행 결과(런별 폴더), uploads/   업로드 파일
```

## 2. CLI 계약 (`depth_sim.exe`)
```
depth_sim.exe run   --in <file> --out <dir> [--ref <file>] [옵션]
depth_sim.exe batch --folder <tire folder> --out <dir> [--fovproc <FOVPROC.ini>] [옵션]
depth_sim.exe info  --in <file>
depth_sim.exe version
```
옵션(모두 선택):
```
--ini <alg_depth_preproc.ini> --cal <N>   ini 의 [CAL000N] 값을 기본값으로 로드(이후 개별 옵션이 덮어씀)
--type INNERCENTER|BEAD|INSHOULDER        지그 제거 단계 선택(현장 DLL 과 동일: 없음 / Bead 최대성분 / 침식 3 최대성분)
--break-kernel N     INSHOULDER 침식 커널, 기본 3(현장 리터럴). 0~2 = 침식 없음(구 최대성분 규칙)
--patch W,H          기본 15,15         --overlap F   기본 0.25(ini 없으면 DLL 기본 0.5 가 아니라 레시피 기본 0.25 를 쓴다; README 명시)
--lower P --upper P  기본 5 / 95 (exp_use_ini_pct=1 일 때만 적용)
--stage AUTO|TOP|BOTTOM|NONE  기본 AUTO  --roi x1,y1,x2,y2  기본 미사용
--exp-use-ini-pct 0|1  --exp-valid-pct 0|1  --exp-masked-median 0|1  --exp-null-value N(-1=끄기)  --exp-fill-holes <maxpx>(0=끄기)
--dump min|all       기본 all. min = result.png + stats.json 만
--preview N          미리보기 축소 배율, 기본 8
--px-x um --px-y um  (선택) 화소 크기. 지정 시 stats 에 mm 환산 추가(기본 300/100 = 숄더 Gocator)
```
`run` 출력(<out> 아래, 파일명 고정):
```
result.png            8U 결과(DLL 출력과 동일 값)          result_f32.tif   DLL 출력 버퍼 형식(float32, 값 0..255)
raw_vis.png           raw 를 1~99 백분위로 8U 정규화, null=빨강(BGR 컬러 PNG)
null_mask.png         원본 null(-999) 255                   stage_removed_mask.png   removeStage 가 -999 로 바꾼 픽셀 255
scaled.png            scaleTo16bitIgnoreNull 결과를 16U 로 (0..65535 clamp)
basis.png             기준면(16U)                            diff.png   diff 를 [-1000,1000] → 0..255 선형 매핑(128=0)
clipped.png           postClipNormalize 클립 후 정규화 전 값을 [low,high]→0..255
ref.png               --ref 가 있으면 참조 8U               ref_diff.png   |result-ref| ×8 (0..255 clamp), 불일치 강조
preview_*.png         위 각 이미지의 1/N 축소본(UI 용)
raw.f32 basis.f32 diff.f32   row-major little-endian float32 원본 크기 덤프(--dump all 일 때; UI 픽셀 인스펙터용)
stats.json            아래 스키마
```
`stats.json` 스키마(키 이름 고정):
```json
{
 "tool_version": "…", "input": {"path": "…", "width": 8192, "height": 1940, "dtype": "float32"},
 "params_effective": {"patch_w":15,"patch_h":15,"overlap":0.25,"step_w":11,"step_h":11,"lower":5,"upper":95,
                      "stage":"AUTO","roi":null,"exp":{"use_ini_pct":false,"valid_pct":false,"masked_median":false,"null_value":-1,"fill_holes":0},
                      "dll_identical": true},
 "timing_ms": {"read":0,"remove_stage":0,"scale":0,"basis":0,"diff":0,"clip_normalize":0,"total":0},
 "null": {"count":0,"pct":0.0,"stage_removed_count":0,"stage_removed_pct":0.0,"valid_after_stage":0,
          "valid_components":0,"largest_component_pct_of_valid":0.0},
 "valid_rows": {"first":0,"last":0},
 "z": {"min":0.0,"max":0.0,"unit_mm_per_scaled_unit":0.0},
 "clip": {"low":0.0,"high":0.0,"low_mm":0.0,"high_mm":0.0,"norm_min":0.0,"norm_max":0.0,"zero_diff_pct":0.0},
 "output": {"black_count":0,"black_pct":0.0,"black_in_valid":0,"black_in_valid_pct":0.0,"white_in_valid":0,"white_in_valid_pct":0.0,
            "row_profile_bins":20,"black_pct_by_valid_row_bin":[…20],"white_pct_by_valid_row_bin":[…20]},
 "diff_hist": {"min":-1000,"max":1000,"bins":256,"counts":[…], "valid_only":true},
 "ref": {"path":"…","exact_pct":0.0,"within1_pct":0.0,"within2_pct":0.0,"max_abs":0,"mismatch_count":0,
         "black_mask_iou":0.0,"ref_black_not_result":0,"result_black_not_ref":0}   // --ref 없으면 null
}
```
`batch`: 폴더의 `*_D.mim`(이름에 `_Proc` 없는 depth) 를 FOVPROC 매핑(기본: 1→9, 3→10, 5→11, 7→12, 13→21, 15→22, 17→23, 19→24;
`--fovproc` 지정 시 RequireImgIdx→ResultImgIdx, ParamIdx→CAL 로 대체)으로 처리하고 참조 `_Proc` 가 있으면 자동 비교.
출력 `<out>\<imgIdx>_<name>\` 에 run 과 동일 파일 + `<out>\batch.json`(이미지별 stats 요약 배열).
종료 코드: 0 성공, 2 인자 오류, 3 입력 읽기 실패, 4 처리 예외. 에러는 stderr 에 한 줄 JSON `{"error":"…"}`.

## 3. 실험 옵션 정의(기본 경로와 분리된 함수)
- `exp_valid_pct`: 백분위를 null 제외 유효 픽셀로 계산(`computePercentile` 대신 마스크 버전).
- `exp_masked_median`: 패치 median 에서 null(0) 제외, 전부 null 인 패치는 최근접 유효 격자값으로 채움.
- `exp_null_value N`: 최종 8U 에서 null 픽셀 값을 N 으로(기본 -1 = DLL 처럼 0).
- `exp_fill_holes maxpx`: removeStage 뒤, 면적 ≤ maxpx 인 null 성분을 인페인팅(cv::inpaint TELEA, 정규화 8U 경유)으로 채운 뒤 처리.
- `exp_use_ini_pct`: lower/upper 지정값 사용.
어느 하나라도 켜지면 `params_effective.dll_identical=false` 로 표기하고 UI 에 경고 배지를 띄운다.

## 4. UI 계약 (`node ui/server.js [--port 8765] [--cli <exe>] [--root <기본 탐색 루트>]`)
- 의존성: Node 내장 모듈만(http, fs, path, child_process, url). npm install 불필요.
- API
  - `GET /` 정적 UI. `GET /api/config` → `{cli, roots[], presets:[{name,cal,type,patch,overlap,lower,upper,stage,roi}], mapping}`
    (presets 는 `--ini` 로 넘긴 alg_depth_preproc.ini 에서 로드, 없으면 레시피 기본 3종 하드코딩)
  - `GET /api/browse?path=` → `{dirs:[], files:[{name,size,kind}]}` (.mim/.tif/.png/.bmp/.jpg)
  - `POST /api/run` body `{input, ref, params}` → CLI `run` 실행 → `{runId, outDir, stats, files:{result:"/runs/<id>/result.png",…}}`
  - `POST /api/batch` body `{folder, params, fovproc}` → `{batchId, items:[{imgIdx,name,stats,files}]}`
  - `GET /api/pixel?runId&x&y` → `{raw, basis, diff, result, ref}` (f32 덤프에서 서버가 읽음; 덤프 없으면 null)
  - `PUT /api/upload?name=` (raw body) → `{path}`
  - `GET /runs/<id>/<file>` 정적 제공
- 화면
  - 좌: 입력 선택(경로 입력 + 서버 폴더 탐색 + 드래그앤드롭 업로드), 참조(_Proc) 자동 추정(같은 폴더의 매핑 인덱스) 및 수동 지정,
    파라미터 폼(프리셋 선택, PatchSize W/H, Overlap, StagePosition, ROI, Lower/Upper + 실험 토글), [실행], [A/B 고정].
  - 우: 패널 그리드(체크박스로 레이어 선택: raw(null 빨강)/null mask/stage removed/scaled/basis/diff/clipped/result/ref/ref_diff),
    **모든 패널 동기 줌·팬**(휠 줌, 드래그 팬, 더블클릭 리셋), 마우스 위치 픽셀 인스펙터(x,y, raw z, basis, diff, result, ref),
    통계 표(null %, 지그 제거 %, 띠 행 범위, low/high(단위·mm), 검정/흰색 %, 참조 일치 %), diff 히스토그램(canvas, low/high 선),
    행 구간별 검정 % 막대. 배치 탭: 타이어 폴더 지정 → 8장 결과·참조·일치율 그리드.
  - 한국어 라벨, 격식체. 외부 CDN 사용 금지(오프라인 동작).

## 5. 검증 계약 (`tests/verify.py`, `tests/cases.json`)
데이터: `H:\000. PJT\01. HankookTire\03_이미지\PC3\1639390184_2000026091709423060`(tire1),
`H:\000. PJT\01. HankookTire\03_이미지\PC3\2000026091715281105`(tire2). raw 5/7/17/19 ↔ 현장 _Proc 11/12/23/24, 3↔10, 1↔9, 13↔21, 15↔22.
레시피: `D:\AIV\MODEL\[1]TireInspect_PC3_DEPLOY\alg_depth_preproc.ini`(CAL1 INNERCENTER 50,50/0.1, CAL2 BEAD 15,15/0.25, CAL3 INSHOULDER 15,15/0.25), `FOVPROC.ini`.
파이썬 레퍼런스: `C:\Users\AIV\Desktop\PC3_DepthPreproc_Null검토\근거자료\simulate.py`(withStage 경로가 현장 출력과 검정 마스크 99.6%, ±2 이내 96.9%).
- T1 tire1 4개 숄더 + 비드 + 센터: 기본 파라미터(각 CAL) → 현장 _Proc 대비 exact_pct, within1, black_mask_iou 기록.
  기대(현장 소스 기준): **exact 100%(불일치 0 px), IoU 1.0**. 2026-09-18 실측: 4쌍 모두 100% 동일. exact 가 100% 미만이면 회귀로 본다.
- T2 같은 입력 `--stage NONE` → T1 보다 크게 나빠져야 함(removeStage 존재 확인).
- T3 파이썬 레퍼런스(withStage) 대비 within2 ≥ 96%.
- T4 tire2 batch 정상 종료·8장 stats 생성. T5 CLI 에러 경로(없는 파일 → exit 3, 잘못된 patch → exit 2).
- T6 UI 스모크: 서버 기동 → /api/config, /api/run(tire1 7→12), /api/pixel, /api/batch(tire2) 응답 검증 → 종료.

## 6. 참고: 원본 INSPECT 흐름(0a6814f2)
```
src32f = MbufGet(float)            → cv::Mat CV_32FC1 (연속 메모리)
useRoi = roi[2]>roi[0] && roi[3]>roi[1];  if useRoi: src32f = src32f(roi & imageRect)   (뷰)
removeStageFromRawData(src32f, m_nStagePos)   // 3(NONE)이면 생략, 그 외 최대 성분 유지
result8u = processHighCurvature(src32f, m_PatchSize, m_overlap)   // 내부 postClipNormalize(…,5,95)
finalImage = useRoi ? zeros(originalSize) 에 result8u 를 roi 위치에 복사 : result8u
출력 MIL 버퍼가 32-bit float 이면 8U→32F 로 변환해 MbufPut (값 0..255)
```
주의: 커밋 0a6814f2 의 `MbufSave(L"E:\\debug_input_after_proc.mim")` 디버그 저장은 현장 소스에서는 주석 처리돼 있으며
시뮬레이터에도 포함하지 않는다. 현장 INSPECT 의 지그 제거는 위 §0-1 의 타입별 규칙을 따른다(0a6814f2 는 모든 타입에
구 최대성분 규칙을 적용해 현장과 다르다).
