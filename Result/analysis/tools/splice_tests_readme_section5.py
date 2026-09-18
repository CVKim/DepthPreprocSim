# Replace tests/README.md section 5 (stand-in harness results, now stale) with a pointer to the real results.
import io
P = r"E:\Dev\DepthPreprocSim\tests\README.md"
s = io.open(P, encoding="utf-8").read()
start = s.index("## 5. fake_exe.py")
end = s.index("## 6. 주의 사항")
new = """## 5. fake_exe.py (파이썬 스탠드인) — 하네스 개발용

`tests/fake_exe.py` 는 `depth_sim.exe` 와 같은 CLI 계약(`run`/`batch`/`info`/`version`, 종료 코드 0/2/3/4, stderr 한 줄 JSON,
`result.png`+`stats.json`(`ref` 블록), `batch.json`, `<imgIdx>_<name>` 폴더)을 `simulate.py` 위에 흉내 낸 것으로, C++ 빌드가 없을 때
하네스(`verify.py`) 자체를 점검하는 용도다. DLL 과 비트 동일하지 않으므로 **검증 수치의 근거로 쓰지 않는다**.

```
D:\\anaconda\\python.exe tests\\verify.py --exe tests\\fake_exe.py --out tests\\results_fake
```

**최종 검증 수치는 `tests/results/RESULTS.md`(C++ exe, 현장 PC3 소스 기준) 를 본다.** 2026-09-18 결과: T1 정상 표본 6장·T4 이상 표본 8장 모두
현장 `_Proc` 와 exact 100.000 %(불일치 0 px, 검정 IoU 1.0), T2 `--stage NONE` 하락 49~56 pp, T3 파이썬 레퍼런스 대비 99.98~99.99 %,
T5 exit 2/3 규약 확인, T6 UI 스모크 12/12 PASS. 임계값은 `cases.json` 의 `expect`(T1/T4 exact ≥ 99.9, 검정 IoU ≥ 0.99, T2 하락 ≥ 20 pp,
T3 ±2 이내 ≥ 96 %) 에 있고, T4 는 항목·개별 run 모두 T1 과 같은 임계로 판정한다.

"""
s2 = s[:start] + new + s[end:]
io.open(P, "w", encoding="utf-8", newline="\n").write(s2)
print("replaced", end - start, "->", len(new), "chars")
