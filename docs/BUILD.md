> Detailed reference (Korean). Back to the [README](../README.md).

# 빌드

## 요구 사항
- Windows 11 x64, Visual Studio 의 **v142 툴셋(MSVC 14.29.30133)**
  - VS 18 Insiders 에 v142 가 함께 설치되어 있으면 그대로 사용합니다(`-G "Visual Studio 18 2026" -A x64 -T v142`).
  - 대안: VS 2019(`-G "Visual Studio 16 2019" -A x64`).
- CMake 3.20 이상(VS 번들 cmake 4.3.1 또는 시스템 cmake 4.0.1).
- OpenCV 4.4.0 사전 빌드: `C:\Program Files\AIV\ThirdParty\opencv\opencv_440`
  (`include\`, `lib\x64_Release\opencv_world440.lib`, `bin\x64_Release\opencv_world440.dll`, Debug 는 `x64_Debug\opencv_world440d.*`).
  다른 위치이면 `-DOPENCV_ROOT=<경로>` 로 지정합니다.

DLL 과 같은 OpenCV 바이너리·같은 툴셋(v142)·같은 부동소수점 모델(`/fp:precise`)·같은 최적화(Release `/O2 /Oi /GL` + `/LTCG`)를
사용해야 비트 동일성이 보장됩니다.

## 빌드 방법
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

## 소스 구성
| 경로 | 내용 |
|---|---|
| `core/DepthPreprocCore.h/.cpp` | `namespace DepthProcessor` 7개 함수를 **현장(PC3) 빌드 소스**(`reference/site_pc3_20260708/3dDepthProcessing.cpp`, 2026-07-08, rc 1.0.2.0.38cf930_HT)에서 **그대로 복사**. `SimPipeline::Run` 이 `C3DPreprocess::INSPECT` 흐름(ROI 뷰 → 타입별 지그 제거 → processHighCurvature → ROI 복원)을 재현하고 중간 단계를 캡처. 실험 경로는 `DepthProcessorExp` 로 분리 |
| `reference/site_pc3_20260708/` | 현장 PC3 에서 실제로 돌아가는 `alg_depth_preproc` 소스 사본(cpp/h/rc). talos-platform 커밋 0a6814f2 와는 지그 제거 단계만 다름(§3.3) |
| `core/MimReader.h/.cpp` | MIL `.mim`(little-endian TIFF, 무압축, 단일 채널, float32/uint8/uint16, 다중 strip) 읽기, float32 단일 strip TIFF 쓰기. 그 외 확장자는 `cv::imdecode(IMREAD_UNCHANGED)` |
| `core/IniReader.h/.cpp` | `alg_depth_preproc.ini` `[CALxxxx]`, `FOVPROC.ini` `[FOVPROCxxxx]` 파서(DLL `ReadSection` 과 같은 규칙) |
| `core/Stats.h/.cpp` | 통계·참조 비교·히스토그램·`stats.json` 직렬화(외부 라이브러리 없음) |
| `cli/main.cpp` | `depth_sim.exe` (wmain, 한글 경로 지원) |
