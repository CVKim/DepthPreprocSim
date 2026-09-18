import sys, re
for path in sys.argv[1:]:
    b = open(path, "rb").read()
    print("=====", path, len(b), "bytes")
    for s in ["StagePosition", "debug_input_after_proc", "test_debug_input", "AUTO", "NONE", "BOTTOM",
              "Lower Percentage", "Overlap", "PatchSize", "DepthPreprocType", "result8u is empty",
              "Unknown eDepthPreprocType", "Not implemented for this milType"]:
        w = s.encode("utf-16-le")
        a = s.encode("ascii")
        print(f"  {s!r:34} utf16={b.count(w)} ascii={b.count(a)}")
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){6,}", b):
        t = m.group().decode("utf-16-le", "ignore")
        if "E:\\" in t or ".mim" in t or ".tif" in t:
            print("  wide string:", t)
    for m in re.finditer(rb"[\x20-\x7e]{6,}", b):
        t = m.group().decode("ascii", "ignore")
        if ".mim" in t or ".tif" in t or "E:\\" in t:
            print("  ascii string:", t)
