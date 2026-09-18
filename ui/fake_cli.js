'use strict';
/*
 * fake_cli.js - development stand-in for depth_sim.exe (DESIGN.md §2).
 * Same commands / flags / exit codes / output files, but the image content is SYNTHETIC
 * (the .mim is only probed for its header). It is NOT DLL-identical; use it only to
 * develop and test ui/server.js and the browser UI without the C++ build.
 *
 *   node ui/fake_cli.js run   --in <file> --out <dir> [--ref <file>] [options]
 *   node ui/fake_cli.js batch --folder <dir> --out <dir> [--fovproc <ini>] [options]
 *   node ui/fake_cli.js info  --in <file>
 *   node ui/fake_cli.js version
 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const TOOL_VERSION = 'depth_sim FAKE 0.1.0 (ui/fake_cli.js, synthetic data, not DLL-identical)';
const NULLV = -999;
const STAGES = ['AUTO', 'TOP', 'BOTTOM', 'NONE'];
const TYPES = ['INNERCENTER', 'BEAD', 'INSHOULDER'];
const DEFAULT_MAPPING = {
  1: { result: 9, cal: 1, name: 'InnerCenter' }, 3: { result: 10, cal: 2, name: 'Bead' },
  5: { result: 11, cal: 3, name: 'InShoulder_L' }, 7: { result: 12, cal: 3, name: 'InShoulder_R' },
  13: { result: 21, cal: 1, name: 'InnerCenter' }, 15: { result: 22, cal: 2, name: 'Bead' },
  17: { result: 23, cal: 3, name: 'InShoulder_L' }, 19: { result: 24, cal: 3, name: 'InShoulder_R' },
};
const DEFAULT_CAL = {
  1: { type: 'INNERCENTER', patch: [50, 50], overlap: 0.1 },
  2: { type: 'BEAD', patch: [15, 15], overlap: 0.25 },
  3: { type: 'INSHOULDER', patch: [15, 15], overlap: 0.25 },
};

function fail(code, msg) {
  process.stderr.write(JSON.stringify({ error: msg }) + '\n');
  process.exit(code);
}

// ---------------------------------------------------------------- args
function parseArgs(argv) {
  const cmd = argv[0];
  const o = {};
  for (let i = 1; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) fail(2, `unexpected argument: ${a}`);
    const key = a.slice(2);
    if (i + 1 >= argv.length) fail(2, `missing value for --${key}`);
    o[key] = argv[++i];
  }
  return { cmd, o };
}
function parseIni(text) {
  const sections = {};
  let cur = null;
  for (const raw of text.replace(/^﻿/, '').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith(';') || line.startsWith('#')) continue;
    const s = /^\[(.+)\]$/.exec(line);
    if (s) { cur = {}; sections[s[1].trim().toUpperCase()] = cur; continue; }
    const eq = line.indexOf('=');
    if (cur && eq > 0) cur[line.slice(0, eq).trim().toUpperCase()] = line.slice(eq + 1).trim();
  }
  return sections;
}
function readIniFile(file) {
  const buf = fs.readFileSync(file);
  const text = buf.length >= 2 && buf[0] === 0xff && buf[1] === 0xfe ? buf.toString('utf16le', 2) : buf.toString('utf8');
  return parseIni(text);
}
function intList(s, n) {
  const parts = String(s).split(',').map((v) => parseInt(v.trim(), 10));
  return parts.length === n && parts.every(Number.isFinite) ? parts : null;
}
function boolFlag(v, name) {
  if (v === undefined) return false;
  if (v === '0' || v === '1') return v === '1';
  fail(2, `--${name} expects 0 or 1`);
  return false;
}
function defaultParams() {
  return {
    type: 'INSHOULDER', patch_w: 15, patch_h: 15, overlap: 0.25, lower: 5, upper: 95, stage: 'AUTO', roi: null,
    exp: { use_ini_pct: false, valid_pct: false, masked_median: false, null_value: -1, fill_holes: 0 },
    dump: 'all', preview: 8, px_x: 300, px_y: 100, px_given: false,
  };
}
function applyCalSection(p, sec, label) {
  if (!sec) fail(2, `ini section ${label} not found`);
  const t = (sec['DEPTHPREPROCTYPE'] || '').toUpperCase();
  if (TYPES.includes(t)) p.type = t;
  const patch = sec['PATCHSIZE'] ? intList(sec['PATCHSIZE'], 2) : null;
  if (patch) { p.patch_w = patch[0]; p.patch_h = patch[1]; }
  if (sec['OVERLAP'] !== undefined) p.overlap = parseFloat(sec['OVERLAP']); else p.overlap = 0.5;
  if (sec['LOWER PERCENTAGE'] !== undefined) p.lower = parseFloat(sec['LOWER PERCENTAGE']);
  if (sec['UPPER PERCENTAGE'] !== undefined) p.upper = parseFloat(sec['UPPER PERCENTAGE']);
  const st = (sec['STAGEPOSITION'] || 'AUTO').toUpperCase();
  p.stage = STAGES.includes(st) ? st : 'AUTO';
  const roi = sec['ROI'] ? intList(sec['ROI'], 4) : null;
  p.roi = roi && roi[2] > roi[0] && roi[3] > roi[1] ? roi : null;
}
function resolveParams(o, base) {
  const p = base || defaultParams();
  if (o.ini && o.cal !== undefined) {
    const cal = parseInt(o.cal, 10);
    if (!Number.isFinite(cal)) fail(2, '--cal expects an integer');
    if (!fs.existsSync(o.ini)) fail(3, `ini not found: ${o.ini}`);
    const ini = readIniFile(o.ini);
    applyCalSection(p, ini[`CAL${String(cal).padStart(4, '0')}`], `CAL${String(cal).padStart(4, '0')}`);
  }
  if (o.type !== undefined) {
    const t = o.type.toUpperCase();
    if (!TYPES.includes(t)) fail(2, `--type must be one of ${TYPES.join('|')}`);
    p.type = t;
  }
  if (o.patch !== undefined) {
    const l = intList(o.patch, 2);
    if (!l || l[0] < 1 || l[1] < 1) fail(2, `invalid --patch '${o.patch}' (expected W,H with positive integers)`);
    p.patch_w = l[0]; p.patch_h = l[1];
  }
  if (o.overlap !== undefined) p.overlap = parseFloat(o.overlap);
  if (!(p.overlap > 0 && p.overlap < 1)) fail(2, `invalid overlap ${o.overlap !== undefined ? o.overlap : p.overlap} (expected open interval (0,1))`);
  if (o.lower !== undefined) p.lower = parseFloat(o.lower);
  if (o.upper !== undefined) p.upper = parseFloat(o.upper);
  if (!(Number.isFinite(p.lower) && Number.isFinite(p.upper) && p.lower >= 0 && p.upper <= 100 && p.lower < p.upper)) fail(2, 'invalid --lower/--upper');
  if (o.stage !== undefined) {
    const s = o.stage.toUpperCase();
    if (!STAGES.includes(s)) fail(2, `--stage must be one of ${STAGES.join('|')}`);
    p.stage = s;
  }
  if (o.roi !== undefined) {
    const l = intList(o.roi, 4);
    if (!l) fail(2, `invalid --roi '${o.roi}' (expected x1,y1,x2,y2)`);
    p.roi = l[2] > l[0] && l[3] > l[1] ? l : null;
  }
  p.exp.use_ini_pct = boolFlag(o['exp-use-ini-pct'], 'exp-use-ini-pct') || (o['exp-use-ini-pct'] === undefined && p.exp.use_ini_pct);
  p.exp.valid_pct = boolFlag(o['exp-valid-pct'], 'exp-valid-pct') || (o['exp-valid-pct'] === undefined && p.exp.valid_pct);
  p.exp.masked_median = boolFlag(o['exp-masked-median'], 'exp-masked-median') || (o['exp-masked-median'] === undefined && p.exp.masked_median);
  if (o['exp-null-value'] !== undefined) {
    p.exp.null_value = parseInt(o['exp-null-value'], 10);
    if (!Number.isFinite(p.exp.null_value) || p.exp.null_value > 255) fail(2, '--exp-null-value expects -1..255');
  }
  if (o['exp-fill-holes'] !== undefined) {
    p.exp.fill_holes = parseInt(o['exp-fill-holes'], 10);
    if (!Number.isFinite(p.exp.fill_holes) || p.exp.fill_holes < 0) fail(2, '--exp-fill-holes expects an integer >= 0');
  }
  if (o.dump !== undefined) {
    if (o.dump !== 'min' && o.dump !== 'all') fail(2, '--dump must be min|all');
    p.dump = o.dump;
  }
  if (o.preview !== undefined) {
    p.preview = parseInt(o.preview, 10);
    if (!Number.isFinite(p.preview) || p.preview < 1) fail(2, '--preview expects an integer >= 1');
  }
  if (o['px-x'] !== undefined || o['px-y'] !== undefined) {
    p.px_x = parseFloat(o['px-x']); p.px_y = parseFloat(o['px-y']);
    if (!(p.px_x > 0 && p.px_y > 0)) fail(2, '--px-x/--px-y expect positive numbers (um)');
    p.px_given = true;
  }
  p.step_w = Math.max(1, Math.trunc(p.patch_w * (1 - p.overlap)));
  p.step_h = Math.max(1, Math.trunc(p.patch_h * (1 - p.overlap)));
  p.dll_identical = !(p.exp.use_ini_pct || p.exp.valid_pct || p.exp.masked_median || p.exp.null_value !== -1 || p.exp.fill_holes > 0);
  return p;
}

// ---------------------------------------------------------------- TIFF header probe (real .mim files)
function probeTiff(file) {
  let fd;
  try { fd = fs.openSync(file, 'r'); } catch (e) { return null; }
  try {
    const head = Buffer.alloc(16);
    if (fs.readSync(fd, head, 0, 16, 0) < 8) return null;
    const le = head[0] === 0x49 && head[1] === 0x49;
    const be = head[0] === 0x4d && head[1] === 0x4d;
    if (!le && !be) return null;
    const r16 = (b, o) => (le ? b.readUInt16LE(o) : b.readUInt16BE(o));
    const r32 = (b, o) => (le ? b.readUInt32LE(o) : b.readUInt32BE(o));
    if (r16(head, 2) !== 42) return null;
    const ifd = r32(head, 4);
    const cnt = Buffer.alloc(2);
    fs.readSync(fd, cnt, 0, 2, ifd);
    const n = r16(cnt, 0);
    const ents = Buffer.alloc(n * 12);
    fs.readSync(fd, ents, 0, n * 12, ifd + 2);
    const tags = {};
    for (let i = 0; i < n; i++) {
      const tag = r16(ents, i * 12), type = r16(ents, i * 12 + 2), count = r32(ents, i * 12 + 4);
      let val;
      if (type === 3 && count === 1) val = r16(ents, i * 12 + 8);
      else if ((type === 4 || type === 3) && count === 1) val = r32(ents, i * 12 + 8);
      else if (type === 3 && count > 1 && count <= 2) val = r16(ents, i * 12 + 8);
      else val = r32(ents, i * 12 + 8);
      tags[tag] = val;
    }
    const bits = tags[258] || 8, sf = tags[339] || 1;
    const dtype = sf === 3 ? (bits === 64 ? 'float64' : 'float32') : (bits === 16 ? 'uint16' : bits === 32 ? 'uint32' : 'uint8');
    return { width: tags[256], height: tags[257], bits, sample_format: sf, compression: tags[259] || 1, dtype };
  } catch (e) { return null; } finally { try { fs.closeSync(fd); } catch (e) { /* ignore */ } }
}

// ---------------------------------------------------------------- synthetic raw
function hashStr(s) { let h = 2166136261; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }
function mulberry32(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
function makeRaw(W, H, seed) {
  const rnd = mulberry32(seed);
  const gauss = () => { const u = 1 - rnd(), v = rnd(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); };
  const raw = new Float32Array(W * H).fill(NULLV);
  const top = 14 + Math.floor(rnd() * 8), bottom = H - (10 + Math.floor(rnd() * 8));
  const Hb = Math.max(1, bottom - top);
  const tilt = (rnd() - 0.5) * 0.06, curv = 220 + rnd() * 160, base = 400 + rnd() * 100;
  for (let y = top; y < bottom; y++) {
    for (let x = 0; x < W; x++) raw[y * W + x] = base + curv * Math.sin(Math.PI * (y - top) / Hb) + tilt * x + 2.5 * gauss();
  }
  // Jig/stage block on the left, separated from the tire by a null gap (removed by removeStage).
  const stageW = 60 + Math.floor(rnd() * 30);
  for (let y = top + 3; y < bottom - 3; y++) {
    for (let x = 0; x < stageW; x++) raw[y * W + x] = 150 + 0.4 * (y - top) + gauss();
    for (let x = stageW; x < stageW + 4; x++) raw[y * W + x] = NULLV;
  }
  for (let x = 0; x < stageW + 4; x++) { for (let y = top; y < top + 3; y++) raw[y * W + x] = NULLV; for (let y = bottom - 3; y < bottom; y++) raw[y * W + x] = NULLV; }
  // Small isolated island near the right edge (second minor component).
  const ix0 = W - 60, iy0 = top + 2;
  for (let y = iy0 - 1; y < iy0 + 9; y++) for (let x = ix0 - 1; x < ix0 + 21; x++) if (y >= 0 && y < H && x >= 0 && x < W) raw[y * W + x] = NULLV;
  for (let y = iy0; y < iy0 + 8; y++) for (let x = ix0; x < ix0 + 20; x++) raw[y * W + x] = base + 30 + gauss();
  // Random dropouts (single pixels and small clusters) inside the tire region.
  const nHoles = Math.floor(W * Hb * 0.004);
  for (let i = 0; i < nHoles; i++) { const x = stageW + 6 + Math.floor(rnd() * (W - stageW - 8)), y = top + Math.floor(rnd() * Hb); raw[y * W + x] = NULLV; }
  for (let k = 0; k < 6; k++) {
    const cx = stageW + 20 + Math.floor(rnd() * (W - stageW - 40)), cy = top + 4 + Math.floor(rnd() * Math.max(1, Hb - 8)), r = 2 + Math.floor(rnd() * 3);
    for (let y = cy - r; y <= cy + r; y++) for (let x = cx - r; x <= cx + r; x++) if (y >= 0 && y < H && x >= 0 && x < W && (x - cx) ** 2 + (y - cy) ** 2 <= r * r) raw[y * W + x] = NULLV;
  }
  // Defects: bumps and dents.
  for (let k = 0; k < 5; k++) {
    const cx = stageW + 30 + Math.floor(rnd() * (W - stageW - 60)), cy = top + 6 + Math.floor(rnd() * Math.max(1, Hb - 12));
    const r = 3 + Math.floor(rnd() * 5), amp = (k % 2 === 0 ? 1 : -1) * (30 + rnd() * 30);
    for (let y = cy - r; y <= cy + r; y++) for (let x = cx - r; x <= cx + r; x++) {
      if (y < 0 || y >= H || x < 0 || x >= W) continue;
      const d2 = (x - cx) ** 2 + (y - cy) ** 2;
      if (d2 <= r * r && raw[y * W + x] > -900) raw[y * W + x] += amp * (1 - d2 / (r * r + 1));
    }
  }
  return raw;
}

// ---------------------------------------------------------------- pipeline (mirrors DepthProcessor on a small image)
function connectedComponents(valid, W, H) {
  const labels = new Int32Array(W * H);
  const areas = [0];
  const stack = new Int32Array(W * H);
  let n = 0;
  for (let i = 0; i < W * H; i++) {
    if (!valid[i] || labels[i]) continue;
    n++;
    let area = 0, sp = 0;
    stack[sp++] = i; labels[i] = n;
    while (sp) {
      const c = stack[--sp];
      area++;
      const cx = c % W, cy = (c - cx) / W;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        if (!dx && !dy) continue;
        const x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        const j = y * W + x;
        if (valid[j] && !labels[j]) { labels[j] = n; stack[sp++] = j; }
      }
    }
    areas.push(area);
  }
  return { labels, areas, count: n };
}
function removeStage(data, W, H, stageMode) {
  const removed = new Uint8Array(W * H);
  const valid = new Uint8Array(W * H);
  for (let i = 0; i < W * H; i++) valid[i] = data[i] > -900 ? 1 : 0;
  const cc = connectedComponents(valid, W, H);
  let best = -1, bestArea = 0;
  for (let l = 1; l <= cc.count; l++) if (cc.areas[l] > bestArea) { bestArea = cc.areas[l]; best = l; }
  if (stageMode !== 'NONE' && best > 0) {
    for (let i = 0; i < W * H; i++) if (valid[i] && cc.labels[i] !== best) { data[i] = NULLV; removed[i] = 1; }
  }
  return { removed, components: cc.count, largestArea: bestArea };
}
function fillSmallHoles(data, W, H, maxPx) {
  const isNull = new Uint8Array(W * H);
  for (let i = 0; i < W * H; i++) isNull[i] = data[i] <= -900 ? 1 : 0;
  const cc = connectedComponents(isNull, W, H);
  let filled = 0;
  for (let i = 0; i < W * H; i++) {
    const l = cc.labels[i];
    if (!l || cc.areas[l] > maxPx) continue;
    const x = i % W, y = (i - x) / W;
    let s = 0, n = 0;
    for (let r = 1; r <= 6 && n === 0; r++) {
      for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) {
        const xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
        const v = data[yy * W + xx];
        if (v > -900 && !isNull[yy * W + xx]) { s += v; n++; }
      }
    }
    if (n) { data[i] = s / n; filled++; }
  }
  return filled;
}
function scaleTo16(data, W, H, isNull) {
  let mn = Infinity, mx = -Infinity, nValid = 0;
  for (let i = 0; i < W * H; i++) if (!isNull[i]) { nValid++; if (data[i] < mn) mn = data[i]; if (data[i] > mx) mx = data[i]; }
  const scaled = new Float32Array(W * H);
  if (nValid && mx > mn) for (let i = 0; i < W * H; i++) scaled[i] = isNull[i] ? 0 : (data[i] - mn) / (mx - mn) * 65536.0;
  return { scaled, mn: nValid ? mn : 0, mx: nValid ? mx : 0 };
}
function median(vals) {
  vals.sort();
  const n = vals.length;
  return n % 2 === 0 ? 0.5 * (vals[n / 2 - 1] + vals[n / 2]) : vals[(n - 1) / 2];
}
function patchMedian(scaled, W, H, p, masked) {
  const outH = Math.max(1, Math.floor((H - p.patch_h) / p.step_h) + 1);
  const outW = Math.max(1, Math.floor((W - p.patch_w) / p.step_w) + 1);
  const sampled = new Float32Array(outW * outH);
  const has = new Uint8Array(outW * outH);
  const buf = new Float32Array(p.patch_w * p.patch_h);
  for (let i = 0; i < outH; i++) {
    for (let j = 0; j < outW; j++) {
      const r0 = i * p.step_h, c0 = j * p.step_w;
      let k = 0;
      for (let rr = 0; rr < p.patch_h; rr++) {
        const y = Math.min(H - 1, r0 + rr);
        for (let cc = 0; cc < p.patch_w; cc++) {
          const v = scaled[y * W + Math.min(W - 1, c0 + cc)];
          if (masked && v === 0) continue;
          buf[k++] = v;
        }
      }
      if (k) { sampled[i * outW + j] = median(buf.subarray(0, k)); has[i * outW + j] = 1; }
    }
  }
  if (masked) {
    // Fill all-null grid cells with the nearest valid grid value.
    for (let i = 0; i < outH; i++) for (let j = 0; j < outW; j++) {
      if (has[i * outW + j]) continue;
      let best = 0, bestD = Infinity;
      for (let a = 0; a < outH; a++) for (let b = 0; b < outW; b++) if (has[a * outW + b]) { const d = (a - i) ** 2 + (b - j) ** 2; if (d < bestD) { bestD = d; best = sampled[a * outW + b]; } }
      sampled[i * outW + j] = best;
    }
  }
  return resizeBilinear(sampled, outW, outH, W, H);
}
function resizeBilinear(src, sw, sh, dw, dh) {
  const out = new Float32Array(dw * dh);
  const sx = sw / dw, sy = sh / dh;
  for (let y = 0; y < dh; y++) {
    let fy = (y + 0.5) * sy - 0.5; if (fy < 0) fy = 0;
    let y0 = Math.floor(fy); if (y0 >= sh - 1) { y0 = sh - 1; fy = y0; }
    const y1 = Math.min(sh - 1, y0 + 1), wy = fy - y0;
    for (let x = 0; x < dw; x++) {
      let fx = (x + 0.5) * sx - 0.5; if (fx < 0) fx = 0;
      let x0 = Math.floor(fx); if (x0 >= sw - 1) { x0 = sw - 1; fx = x0; }
      const x1 = Math.min(sw - 1, x0 + 1), wx = fx - x0;
      out[y * dw + x] = (src[y0 * sw + x0] * (1 - wx) + src[y0 * sw + x1] * wx) * (1 - wy) + (src[y1 * sw + x0] * (1 - wx) + src[y1 * sw + x1] * wx) * wy;
    }
  }
  return out;
}
function percentile(sorted, pct) {
  if (!sorted.length) return 0;
  const pos = pct / 100 * (sorted.length - 1);
  const lo = Math.floor(pos), hi = Math.ceil(pos);
  return sorted[lo] + (pos - lo) * (sorted[hi] - sorted[lo]);
}
function runPipeline(raw, W, H, p) {
  const t = {};
  let t0 = Date.now();
  const nullMask = new Uint8Array(W * H);
  let nullCount = 0;
  for (let i = 0; i < W * H; i++) if (raw[i] <= -900) { nullMask[i] = 1; nullCount++; }
  const data = Float32Array.from(raw);
  const st = removeStage(data, W, H, p.stage);
  let filled = 0;
  if (p.exp.fill_holes > 0) filled = fillSmallHoles(data, W, H, p.exp.fill_holes);
  const isNull = new Uint8Array(W * H);
  let validAfter = 0;
  for (let i = 0; i < W * H; i++) { isNull[i] = data[i] <= -900 ? 1 : 0; if (!isNull[i]) validAfter++; }
  t.remove_stage = Date.now() - t0; t0 = Date.now();
  const { scaled, mn, mx } = scaleTo16(data, W, H, isNull);
  t.scale = Date.now() - t0; t0 = Date.now();
  const basis = patchMedian(scaled, W, H, p, p.exp.masked_median);
  t.basis = Date.now() - t0; t0 = Date.now();
  const diff = new Float32Array(W * H);
  for (let i = 0; i < W * H; i++) { let v = scaled[i] - basis[i]; if (v < -32768) v = -32768; else if (v > 32767) v = 32767; diff[i] = Math.trunc(v); }
  t.diff = Date.now() - t0; t0 = Date.now();
  const pctSrc = p.exp.valid_pct ? diff.filter((_, i) => !isNull[i]) : Float32Array.from(diff);
  pctSrc.sort();
  const lowerPct = p.exp.use_ini_pct ? p.lower : 5, upperPct = p.exp.use_ini_pct ? p.upper : 95;
  let low = Math.max(percentile(pctSrc, lowerPct), -1000), high = Math.min(percentile(pctSrc, upperPct), 1000);
  const clipped = new Float32Array(W * H);
  let cmn = Infinity, cmx = -Infinity;
  for (let i = 0; i < W * H; i++) { const v = Math.min(high, Math.max(low, diff[i])); clipped[i] = v; if (v < cmn) cmn = v; if (v > cmx) cmx = v; }
  const result = new Uint8Array(W * H);
  const rng = cmx > cmn ? cmx - cmn : 1;
  for (let i = 0; i < W * H; i++) result[i] = isNull[i] ? (p.exp.null_value >= 0 ? p.exp.null_value : 0) : Math.max(0, Math.min(255, Math.round((clipped[i] - cmn) * 255 / rng)));
  t.clip_normalize = Date.now() - t0;
  return { nullMask, nullCount, stageRemoved: st.removed, components: st.components, largestArea: st.largestArea, validAfter, isNull, scaled, zmin: mn, zmax: mx, basis, diff, low, high, cmn, cmx, clipped, result, timing: t, filled };
}

// ---------------------------------------------------------------- encoders (PNG / TIFF / f32)
const CRC_TABLE = (() => { const t = new Int32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c; } return t; })();
function crc32(buf) { let c = -1; for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; }
function pngChunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td), 0);
  return Buffer.concat([len, td, crc]);
}
function encodePng(width, height, channels, bitDepth, data) {
  const colorType = channels === 1 ? 0 : channels === 3 ? 2 : 6;
  const stride = width * channels * (bitDepth / 8);
  const rows = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) { rows[y * (stride + 1)] = 0; Buffer.from(data.buffer, data.byteOffset + y * stride, stride).copy(rows, y * (stride + 1) + 1); }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4); ihdr[8] = bitDepth; ihdr[9] = colorType; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), pngChunk('IHDR', ihdr), pngChunk('IDAT', zlib.deflateSync(rows, { level: 6 })), pngChunk('IEND', Buffer.alloc(0))]);
}
function writePng8(file, W, H, u8) { fs.writeFileSync(file, encodePng(W, H, 1, 8, u8)); }
function writePng16(file, W, H, u16) { const b = new Uint8Array(W * H * 2); for (let i = 0; i < W * H; i++) { b[2 * i] = u16[i] >> 8; b[2 * i + 1] = u16[i] & 255; } fs.writeFileSync(file, encodePng(W, H, 1, 16, b)); }
function writePngRgb(file, W, H, rgb) { fs.writeFileSync(file, encodePng(W, H, 3, 8, rgb)); }
function writeTiffF32(file, W, H, f32) {
  const nTags = 10;
  const ifdOff = 8, dataOff = ifdOff + 2 + nTags * 12 + 4;
  const head = Buffer.alloc(dataOff);
  head.write('II', 0, 'latin1'); head.writeUInt16LE(42, 2); head.writeUInt32LE(ifdOff, 4);
  head.writeUInt16LE(nTags, ifdOff);
  const tags = [[256, 4, 1, W], [257, 4, 1, H], [258, 3, 1, 32], [259, 3, 1, 1], [262, 3, 1, 1], [273, 4, 1, dataOff], [277, 3, 1, 1], [278, 4, 1, H], [279, 4, 1, W * H * 4], [339, 3, 1, 3]];
  tags.forEach(([tag, type, count, val], i) => {
    const o = ifdOff + 2 + i * 12;
    head.writeUInt16LE(tag, o); head.writeUInt16LE(type, o + 2); head.writeUInt32LE(count, o + 4);
    if (type === 3) head.writeUInt16LE(val, o + 8); else head.writeUInt32LE(val, o + 8);
  });
  head.writeUInt32LE(0, ifdOff + 2 + nTags * 12);
  fs.writeFileSync(file, Buffer.concat([head, Buffer.from(f32.buffer, f32.byteOffset, f32.byteLength)]));
}
function writeF32(file, f32) { fs.writeFileSync(file, Buffer.from(f32.buffer, f32.byteOffset, f32.byteLength)); }
function downsample(u8, W, H, ch, N) {
  const pw = Math.max(1, Math.floor(W / N)), ph = Math.max(1, Math.floor(H / N));
  const out = new Uint8Array(pw * ph * ch);
  for (let y = 0; y < ph; y++) for (let x = 0; x < pw; x++) { const sy = Math.min(H - 1, y * N), sx = Math.min(W - 1, x * N); for (let c = 0; c < ch; c++) out[(y * pw + x) * ch + c] = u8[(sy * W + sx) * ch + c]; }
  return { pw, ph, out };
}
function toU16(f32) { const u = new Uint16Array(f32.length); for (let i = 0; i < f32.length; i++) u[i] = Math.max(0, Math.min(65535, Math.round(f32[i]))); return u; }
function u16ToU8(u16) { const u = new Uint8Array(u16.length); for (let i = 0; i < u16.length; i++) u[i] = u16[i] >> 8; return u; }

// ---------------------------------------------------------------- stats
function pct(a, b) { return b > 0 ? a / b * 100 : 0; }
function round(v, d) { const f = 10 ** d; return Math.round(v * f) / f; }
function computeStats(inputPath, W, H, p, r, refU8, refPath, timing) {
  const total = W * H;
  let stageRemoved = 0;
  for (let i = 0; i < total; i++) stageRemoved += r.stageRemoved[i];
  let first = -1, last = -1;
  for (let y = 0; y < H; y++) { let any = false; for (let x = 0; x < W; x++) if (!r.isNull[y * W + x]) { any = true; break; } if (any) { if (first < 0) first = y; last = y; } }
  const unit = r.zmax > r.zmin ? (r.zmax - r.zmin) / 65536.0 : 0;
  let zeroDiff = 0, black = 0, blackValid = 0, whiteValid = 0;
  const bins = 20, blackBin = new Array(bins).fill(0), whiteBin = new Array(bins).fill(0), validBin = new Array(bins).fill(0);
  const hist = new Array(256).fill(0);
  const rowsValid = last >= first ? last - first + 1 : 1;
  for (let i = 0; i < total; i++) {
    const v = r.result[i];
    if (v === 0) black++;
    if (r.isNull[i]) continue;
    if (r.diff[i] === 0) zeroDiff++;
    if (v === 0) blackValid++;
    if (v === 255) whiteValid++;
    const y = Math.floor(i / W);
    const b = Math.min(bins - 1, Math.floor((y - first) / rowsValid * bins));
    validBin[b]++; if (v === 0) blackBin[b]++; if (v === 255) whiteBin[b]++;
    const hb = Math.max(0, Math.min(255, Math.floor((r.diff[i] + 1000) / 2000 * 256)));
    hist[hb]++;
  }
  let ref = null;
  if (refU8) {
    let exact = 0, w1 = 0, w2 = 0, maxAbs = 0, inter = 0, uni = 0, refNotRes = 0, resNotRef = 0;
    for (let i = 0; i < total; i++) {
      const d = Math.abs(r.result[i] - refU8[i]);
      if (d === 0) exact++; if (d <= 1) w1++; if (d <= 2) w2++; if (d > maxAbs) maxAbs = d;
      const a = r.result[i] === 0, b = refU8[i] === 0;
      if (a && b) inter++; if (a || b) uni++; if (b && !a) refNotRes++; if (a && !b) resNotRef++;
    }
    ref = { path: refPath, exact_pct: round(pct(exact, total), 4), within1_pct: round(pct(w1, total), 4), within2_pct: round(pct(w2, total), 4), max_abs: maxAbs, mismatch_count: total - exact, black_mask_iou: round(uni ? inter / uni : 1, 6), ref_black_not_result: refNotRes, result_black_not_ref: resNotRef };
  }
  return {
    tool_version: TOOL_VERSION,
    input: { path: inputPath, width: W, height: H, dtype: 'float32' },
    params_effective: {
      patch_w: p.patch_w, patch_h: p.patch_h, overlap: p.overlap, step_w: p.step_w, step_h: p.step_h,
      lower: p.exp.use_ini_pct ? p.lower : 5, upper: p.exp.use_ini_pct ? p.upper : 95, stage: p.stage, roi: p.roi,
      exp: { use_ini_pct: p.exp.use_ini_pct, valid_pct: p.exp.valid_pct, masked_median: p.exp.masked_median, null_value: p.exp.null_value, fill_holes: p.exp.fill_holes },
      dll_identical: p.dll_identical, type: p.type, holes_filled: r.filled, fake: true,
    },
    timing_ms: Object.assign({ read: timing.read, remove_stage: 0, scale: 0, basis: 0, diff: 0, clip_normalize: 0, total: 0 }, r.timing, { total: timing.read + Object.values(r.timing).reduce((a, b) => a + b, 0) }),
    null: {
      count: r.nullCount, pct: round(pct(r.nullCount, total), 4), stage_removed_count: stageRemoved, stage_removed_pct: round(pct(stageRemoved, total), 4),
      valid_after_stage: r.validAfter, valid_components: r.components, largest_component_pct_of_valid: round(pct(r.largestArea, total - r.nullCount), 4),
    },
    valid_rows: { first: Math.max(0, first), last: Math.max(0, last) },
    z: { min: round(r.zmin, 4), max: round(r.zmax, 4), unit_mm_per_scaled_unit: unit },
    clip: { low: r.low, high: r.high, low_mm: round(r.low * unit, 6), high_mm: round(r.high * unit, 6), norm_min: r.cmn, norm_max: r.cmx, zero_diff_pct: round(pct(zeroDiff, r.validAfter), 4) },
    output: {
      black_count: black, black_pct: round(pct(black, total), 4), black_in_valid: blackValid, black_in_valid_pct: round(pct(blackValid, r.validAfter), 4),
      white_in_valid: whiteValid, white_in_valid_pct: round(pct(whiteValid, r.validAfter), 4), row_profile_bins: bins,
      black_pct_by_valid_row_bin: blackBin.map((v, i) => round(pct(v, validBin[i]), 3)), white_pct_by_valid_row_bin: whiteBin.map((v, i) => round(pct(v, validBin[i]), 3)),
    },
    diff_hist: { min: -1000, max: 1000, bins: 256, counts: hist, valid_only: true },
    ref,
  };
}

// ---------------------------------------------------------------- one image
function processOne(inputPath, outDir, refPath, p) {
  const tRead0 = Date.now();
  if (!fs.existsSync(inputPath)) fail(3, `input not found: ${inputPath}`);
  if (refPath && !fs.existsSync(refPath)) fail(3, `reference not found: ${refPath}`);
  const probe = probeTiff(inputPath);
  let W = 1024, H = 200;
  if (probe && probe.width > 0 && probe.height > 0) { W = 1024; H = Math.max(64, Math.min(400, Math.round(probe.height * 1024 / probe.width))); }
  const seed = hashStr(path.basename(inputPath));
  const rawFull = makeRaw(W, H, seed);
  const tRead = Date.now() - tRead0;
  fs.mkdirSync(outDir, { recursive: true });

  // ROI handling as in C3DPreprocess::INSPECT (process the crop, paste into zeros).
  let x0 = 0, y0 = 0, rw = W, rh = H;
  if (p.roi) {
    x0 = Math.max(0, p.roi[0]); y0 = Math.max(0, p.roi[1]);
    rw = Math.min(W, p.roi[2]) - x0; rh = Math.min(H, p.roi[3]) - y0;
    if (rw <= 0 || rh <= 0) fail(4, 'ROI does not intersect the image');
  }
  const rawRoi = new Float32Array(rw * rh);
  for (let y = 0; y < rh; y++) for (let x = 0; x < rw; x++) rawRoi[y * rw + x] = rawFull[(y0 + y) * W + (x0 + x)];
  const rr = runPipeline(rawRoi, rw, rh, p);
  const paste = (src, ctor, fill) => { if (rw === W && rh === H) return src; const out = new ctor(W * H); if (fill) out.fill(fill); for (let y = 0; y < rh; y++) for (let x = 0; x < rw; x++) out[(y0 + y) * W + (x0 + x)] = src[y * rw + x]; return out; };
  const r = {
    nullMask: (() => { const m = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) m[i] = rawFull[i] <= -900 ? 1 : 0; return m; })(),
    nullCount: 0, stageRemoved: paste(rr.stageRemoved, Uint8Array), components: rr.components, largestArea: rr.largestArea, validAfter: rr.validAfter,
    isNull: paste(rr.isNull, Uint8Array, 1), scaled: paste(rr.scaled, Float32Array), zmin: rr.zmin, zmax: rr.zmax, basis: paste(rr.basis, Float32Array),
    diff: paste(rr.diff, Float32Array), low: rr.low, high: rr.high, cmn: rr.cmn, cmx: rr.cmx, clipped: paste(rr.clipped, Float32Array), result: paste(rr.result, Uint8Array), timing: rr.timing, filled: rr.filled,
  };
  for (let i = 0; i < W * H; i++) r.nullCount += r.nullMask[i];

  // Synthetic reference: the result with a few perturbed pixels (the real .mim reference is not decoded here).
  let refU8 = null;
  if (refPath) {
    refU8 = Uint8Array.from(r.result);
    const rnd = mulberry32(seed ^ 0x9e3779b9);
    const n = Math.floor(W * H * 0.004);
    for (let i = 0; i < n; i++) { const k = Math.floor(rnd() * W * H); if (!r.isNull[k]) refU8[k] = Math.max(0, Math.min(255, refU8[k] + (rnd() < 0.5 ? -1 : 1) * (1 + Math.floor(rnd() * 6)))); }
  }
  const stats = computeStats(inputPath, W, H, p, r, refU8, refPath || null, { read: tRead });
  const tw0 = Date.now();
  writePng8(path.join(outDir, 'result.png'), W, H, r.result);
  const previews = {};
  if (p.dump === 'all') {
    const rf = new Float32Array(W * H); for (let i = 0; i < W * H; i++) rf[i] = r.result[i];
    writeTiffF32(path.join(outDir, 'result_f32.tif'), W, H, rf);
    // raw_vis: 1..99 percentile normalization of valid raw, null = red (BGR in OpenCV; RGB here so red is red).
    const vals = rawFull.filter((v) => v > -900); vals.sort();
    const p1 = percentile(vals, 1), p99 = percentile(vals, 99);
    const rgb = new Uint8Array(W * H * 3);
    for (let i = 0; i < W * H; i++) {
      if (rawFull[i] <= -900) { rgb[3 * i] = 255; rgb[3 * i + 1] = 0; rgb[3 * i + 2] = 0; continue; }
      const g = Math.max(0, Math.min(255, Math.round((rawFull[i] - p1) / ((p99 - p1) || 1) * 255)));
      rgb[3 * i] = g; rgb[3 * i + 1] = g; rgb[3 * i + 2] = g;
    }
    writePngRgb(path.join(outDir, 'raw_vis.png'), W, H, rgb); previews.raw_vis = { data: rgb, ch: 3 };
    const nm = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) nm[i] = r.nullMask[i] ? 255 : 0;
    writePng8(path.join(outDir, 'null_mask.png'), W, H, nm); previews.null_mask = { data: nm, ch: 1 };
    const sm = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) sm[i] = r.stageRemoved[i] ? 255 : 0;
    writePng8(path.join(outDir, 'stage_removed_mask.png'), W, H, sm); previews.stage_removed_mask = { data: sm, ch: 1 };
    const s16 = toU16(r.scaled), b16 = toU16(r.basis);
    writePng16(path.join(outDir, 'scaled.png'), W, H, s16); previews.scaled = { data: u16ToU8(s16), ch: 1 };
    writePng16(path.join(outDir, 'basis.png'), W, H, b16); previews.basis = { data: u16ToU8(b16), ch: 1 };
    const d8 = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) d8[i] = Math.max(0, Math.min(255, Math.round((r.diff[i] + 1000) / 2000 * 255)));
    writePng8(path.join(outDir, 'diff.png'), W, H, d8); previews.diff = { data: d8, ch: 1 };
    const c8 = new Uint8Array(W * H); const crng = (r.high - r.low) || 1; for (let i = 0; i < W * H; i++) c8[i] = Math.max(0, Math.min(255, Math.round((r.clipped[i] - r.low) / crng * 255)));
    writePng8(path.join(outDir, 'clipped.png'), W, H, c8); previews.clipped = { data: c8, ch: 1 };
    previews.result = { data: r.result, ch: 1 };
    if (refU8) {
      writePng8(path.join(outDir, 'ref.png'), W, H, refU8); previews.ref = { data: refU8, ch: 1 };
      const rd = new Uint8Array(W * H); for (let i = 0; i < W * H; i++) rd[i] = Math.min(255, Math.abs(r.result[i] - refU8[i]) * 8);
      writePng8(path.join(outDir, 'ref_diff.png'), W, H, rd); previews.ref_diff = { data: rd, ch: 1 };
    }
    for (const [name, pv] of Object.entries(previews)) {
      const { pw, ph, out } = downsample(pv.data, W, H, pv.ch, p.preview);
      if (pv.ch === 3) writePngRgb(path.join(outDir, `preview_${name}.png`), pw, ph, out); else writePng8(path.join(outDir, `preview_${name}.png`), pw, ph, out);
    }
    writeF32(path.join(outDir, 'raw.f32'), rawFull);
    writeF32(path.join(outDir, 'basis.f32'), r.basis);
    writeF32(path.join(outDir, 'diff.f32'), r.diff);
  }
  stats.timing_ms.write = Date.now() - tw0;
  fs.writeFileSync(path.join(outDir, 'stats.json'), JSON.stringify(stats, null, 1));
  return stats;
}

// ---------------------------------------------------------------- commands
function cmdRun(o) {
  if (!o.in) fail(2, 'missing --in');
  if (!o.out) fail(2, 'missing --out');
  const p = resolveParams(o);
  const stats = processOne(o.in, o.out, o.ref || null, p);
  process.stdout.write(JSON.stringify({ out: o.out, width: stats.input.width, height: stats.input.height, total_ms: stats.timing_ms.total, dll_identical: p.dll_identical, fake: true }) + '\n');
}
function loadMapping(fovproc) {
  if (!fovproc) return DEFAULT_MAPPING;
  if (!fs.existsSync(fovproc)) fail(3, `fovproc not found: ${fovproc}`);
  const ini = readIniFile(fovproc);
  const mapping = {};
  for (const [name, sec] of Object.entries(ini)) {
    if (!/^FOVPROC\d+$/.test(name)) continue;
    const req = parseInt(sec['REQUIREIMGIDX'], 10), res = parseInt(sec['RESULTIMGIDX'], 10), cal = parseInt(sec['PARAMIDX'], 10);
    if (Number.isFinite(req) && Number.isFinite(res)) mapping[req] = { result: res, cal: Number.isFinite(cal) ? cal : null, name: sec['NAME'] || `FOV${req}` };
  }
  return Object.keys(mapping).length ? mapping : DEFAULT_MAPPING;
}
function cmdBatch(o) {
  if (!o.folder) fail(2, 'missing --folder');
  if (!o.out) fail(2, 'missing --out');
  let st = null;
  try { st = fs.statSync(o.folder); } catch (e) { st = null; }
  if (!st || !st.isDirectory()) fail(3, `folder not found: ${o.folder}`);
  const mapping = loadMapping(o.fovproc);
  const ini = o.ini ? (fs.existsSync(o.ini) ? readIniFile(o.ini) : fail(3, `ini not found: ${o.ini}`)) : null;
  const files = fs.readdirSync(o.folder).filter((f) => /_D\.mim$/i.test(f) && !/_Proc/i.test(f));
  const items = [];
  fs.mkdirSync(o.out, { recursive: true });
  for (const f of files.sort((a, b) => parseInt(a, 10) - parseInt(b, 10))) {
    const m = /^(\d+)_(.+)\.mim$/i.exec(f);
    if (!m) continue;
    const idx = parseInt(m[1], 10);
    const map = mapping[idx];
    if (!map) continue;
    const base = defaultParams();
    const cal = map.cal || 3;
    if (ini) applyCalSection(base, ini[`CAL${String(cal).padStart(4, '0')}`], `CAL${String(cal).padStart(4, '0')}`);
    else { const d = DEFAULT_CAL[cal] || DEFAULT_CAL[3]; base.type = d.type; base.patch_w = d.patch[0]; base.patch_h = d.patch[1]; base.overlap = d.overlap; }
    const o2 = Object.assign({}, o); delete o2.ini; delete o2.cal;
    const p = resolveParams(o2, base);
    const refName = `${map.result}_${m[2]}_Proc.mim`;
    const refPath = fs.existsSync(path.join(o.folder, refName)) ? path.join(o.folder, refName) : null;
    const sub = path.join(o.out, `${idx}_${map.name}`);
    const t0 = Date.now();
    const stats = processOne(path.join(o.folder, f), sub, refPath, p);
    items.push({
      imgIdx: idx, name: map.name, file: f, ref: refPath ? refName : null, cal, dir: sub, ms: Date.now() - t0,
      stats: { null_pct: stats.null.pct, stage_removed_pct: stats.null.stage_removed_pct, black_in_valid_pct: stats.output.black_in_valid_pct, exact_pct: stats.ref ? stats.ref.exact_pct : null, black_mask_iou: stats.ref ? stats.ref.black_mask_iou : null, dll_identical: stats.params_effective.dll_identical },
    });
  }
  if (!items.length) fail(3, `no *_D.mim with a FOVPROC mapping found in ${o.folder}`);
  fs.writeFileSync(path.join(o.out, 'batch.json'), JSON.stringify(items, null, 1));
  process.stdout.write(JSON.stringify({ out: o.out, count: items.length, fake: true }) + '\n');
}
function cmdInfo(o) {
  if (!o.in) fail(2, 'missing --in');
  if (!fs.existsSync(o.in)) fail(3, `input not found: ${o.in}`);
  const probe = probeTiff(o.in);
  if (!probe) fail(3, `not a TIFF/MIM file: ${o.in}`);
  process.stdout.write(JSON.stringify(Object.assign({ path: o.in, size: fs.statSync(o.in).size }, probe)) + '\n');
}

function main() {
  const { cmd, o } = parseArgs(process.argv.slice(2));
  try {
    if (cmd === 'version') { process.stdout.write(TOOL_VERSION + '\n'); return; }
    if (cmd === 'run') return cmdRun(o);
    if (cmd === 'batch') return cmdBatch(o);
    if (cmd === 'info') return cmdInfo(o);
    fail(2, `unknown command '${cmd || ''}' (expected run|batch|info|version)`);
  } catch (e) {
    fail(4, `processing failed: ${e.message}`);
  }
}
main();
