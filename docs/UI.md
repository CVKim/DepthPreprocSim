> Detailed reference (Korean). Back to the [README](../README.md).

# UI (`ui/server.js` + `ui/public`)

브라우저에서 입력 이미지를 고르고 파라미터를 바꿔 `depth_sim.exe` 를 실행한 뒤, 중간 단계 이미지·통계·참조 비교를 한 화면에서
확인하는 로컬 웹 UI 입니다. Node 내장 모듈(http, fs, path, child_process, zlib)만 사용하므로 `npm install` 이 필요 없고,
외부 CDN·폰트를 쓰지 않아 오프라인에서 동작합니다. 모든 데이터는 로컬 PC 안에서만 오갑니다(기본 바인딩 127.0.0.1).

## 시작
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
| `--root <dir>` | `<images>`, `D:\AIV\MODEL`, `E:\` | 폴더 탐색기의 시작 루트. 여러 번 지정하거나 `;` 로 구분합니다 |
| `--ini <file>` | `<recipe>\alg_depth_preproc.ini` | 프리셋(`[CAL0001..3]`) 원본. 없으면 레시피 기본 3종을 내장값으로 사용합니다 |
| `--fovproc <file>` | `<recipe>\FOVPROC.ini` | `RequireImgIdx → ResultImgIdx / ParamIdx` 매핑. 없으면 1→9, 3→10, 5→11, 7→12, 13→21, 15→22, 17→23, 19→24 를 사용합니다 |
| `--runs <dir>` | `Result\runs\ui\` | 실행 결과 폴더(업로드는 `Result\uploads\`). E: 가 느릴 때 빠른 디스크로 옮길 수 있습니다 |
| `--uploads <dir>` | `uploads\` | 드래그앤드롭 업로드 저장 폴더 |

실행 결과는 `runs\<runId>\` (`runId` = `YYYYMMDD_HHMMSS_NNN`, 배치는 `batch_` 접두)에 CLI 산출물 그대로 저장되고,
서버가 `run.json`(요청 파라미터·명령줄) 과 `cli_stderr.txt` 를 함께 남깁니다. `--dump all` 은 원본 크기 float32 덤프 3장을
포함하므로 숄더(8192×1940) 1회 ≈ 190 MB, 센터(12160×1940) 1회 ≈ 280 MB, 배치 1회 ≈ 1.5 GB 를 차지합니다.
필요 없는 실행 폴더는 그냥 삭제하면 됩니다(서버 재시작 불필요).

## 화면 구성 — 단일 실행 탭
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

## 배치 탭
타이어 폴더(예: `<images>\sampleB`)와 `FOVPROC.ini` 를 지정하고 [배치 실행] 을 누르면 CLI `batch` 가 폴더의 `*_D.mim`
(`_Proc` 제외)을 매핑대로 처리하고 같은 폴더의 `_Proc` 가 있으면 자동 비교합니다. 이미지별 파라미터는 `ParamIdx → [CALxxxx]`
(서버의 `--ini`)를 따르며, “좌측 파라미터를 모든 이미지에 강제 적용” 을 켜면 패치/오버랩/스테이지/ROI 를 모든 이미지에 덮어씁니다.
실험 옵션·덤프·미리보기 설정은 항상 좌측 값을 따릅니다. 결과는 8행 표(인덱스, 이름, 결과·참조 미리보기, 정확 일치 %, ±1 %,
검정 IoU, null %, 유효 내 검정 %, 처리 ms)로 나오고, 행을 클릭하면 그 이미지를 단일 실행 탭에서 열어 레이어·픽셀 인스펙터로 살펴볼 수 있습니다.

## API 요약 (`DESIGN.md` §4)
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

## 개발용 가짜 CLI 와 스모크 테스트
`ui\fake_cli.js` 는 `depth_sim.exe` 와 같은 명령·옵션·종료 코드·산출물 구성을 흉내 내지만 **합성 데이터**를 만드는 개발용 대체물입니다
(결과는 DLL 과 무관하며 `tool_version` 에 FAKE 로 표기). C++ 빌드 없이 UI 를 개발·점검할 때 사용합니다.
```bat
"C:\Program Files\nodejs\node.exe" ui\server.js --port 8765 --cli "node ui\fake_cli.js"
"C:\Program Files\nodejs\node.exe" ui\smoke_test.js --cli "node ui\fake_cli.js"              rem 가짜 CLI 로 전 API 점검
"C:\Program Files\nodejs\node.exe" ui\smoke_test.js --cli build\Release\depth_sim.exe        rem 실제 CLI 로 T6 점검
```
`ui\smoke_test.js` 는 서버를 띄워 `/`, `/api/config`, `/api/browse`, `/api/run`(tire1 7→12), `/runs/...`, `/api/pixel`, `/api/upload`,
`/api/batch`(tire2), 오류 경로(없는 파일 → 404, 잘못된 patch → 400)를 확인한 뒤 서버를 종료합니다.
