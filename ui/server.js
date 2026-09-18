'use strict';
/*
 * DepthPreprocSim UI server (DESIGN.md §4). Node built-ins only, no npm packages.
 *
 *   node ui/server.js [--port 8765] [--host 127.0.0.1] [--cli <depth_sim.exe | node script.js | script.js>]
 *                     [--root <dir>]... [--ini <alg_depth_preproc.ini>] [--fovproc <FOVPROC.ini>]
 *
 * Endpoints: GET / (static UI), GET /api/config, GET /api/browse?path=, POST /api/run, POST /api/batch,
 *            GET /api/pixel?runId&x&y, PUT /api/upload?name=, GET /runs/<id>/<file>
 * Extras   : GET /api/runs (past runs), GET /api/run?runId= (reload one run)
 */
const http = require('http');
const fs = require('fs');
const fsp = fs.promises;
const path = require('path');
const zlib = require('zlib');
const { spawn } = require('child_process');
const { URL } = require('url');

const ROOT = path.resolve(__dirname, '..');
const PUBLIC_DIR = path.join(__dirname, 'public');
// --runs / --uploads override the default folders (e.g. a faster disk); defaults follow DESIGN.md §1.
const EARLY_ARGS = process.argv.slice(2);
const argValue = (name) => { const i = EARLY_ARGS.indexOf(`--${name}`); return i >= 0 && EARLY_ARGS[i + 1] ? EARLY_ARGS[i + 1] : null; };
// Outputs live under Result\ (user request 2026-09-18): UI runs -> Result\runs\ui, uploads -> Result\uploads
const RUNS_DIR = path.resolve(ROOT, argValue('runs') || path.join('Result', 'runs', 'ui'));
const UPLOADS_DIR = path.resolve(ROOT, argValue('uploads') || path.join('Result', 'uploads'));

const DEFAULTS = {
  port: 8765,
  host: '127.0.0.1',
  cli: path.join(ROOT, 'build', 'Release', 'depth_sim.exe'),
  roots: ['H:\\000. PJT\\01. HankookTire\\03_\uC774\uBBF8\uC9C0', 'D:\\AIV\\MODEL', 'E:\\'],
  ini: 'D:\\AIV\\MODEL\\[1]TireInspect_PC3_DEPLOY\\alg_depth_preproc.ini',
  fovproc: 'D:\\AIV\\MODEL\\[1]TireInspect_PC3_DEPLOY\\FOVPROC.ini',
};

// Hardcoded fallbacks (DESIGN.md §2, §5) used when the ini files are missing.
const DEFAULT_MAPPING = {
  1: { result: 9, cal: 1, name: 'InnerCenter' },
  3: { result: 10, cal: 2, name: 'Bead' },
  5: { result: 11, cal: 3, name: 'InShoulder_L' },
  7: { result: 12, cal: 3, name: 'InShoulder_R' },
  13: { result: 21, cal: 1, name: 'InnerCenter' },
  15: { result: 22, cal: 2, name: 'Bead' },
  17: { result: 23, cal: 3, name: 'InShoulder_L' },
  19: { result: 24, cal: 3, name: 'InShoulder_R' },
};
const DEFAULT_PRESETS = [
  { name: 'INNERCENTER', cal: 1, type: 'INNERCENTER', patch: [50, 50], overlap: 0.1, lower: 5, upper: 95, stage: 'AUTO', roi: [9999, 9999, 0, 0] },
  { name: 'BEAD', cal: 2, type: 'BEAD', patch: [15, 15], overlap: 0.25, lower: 5, upper: 95, stage: 'AUTO', roi: [9999, 9999, 0, 0] },
  { name: 'INSHOULDER', cal: 3, type: 'INSHOULDER', patch: [15, 15], overlap: 0.25, lower: 5, upper: 95, stage: 'AUTO', roi: [9999, 9999, 0, 0] },
];
const STAGES = ['AUTO', 'TOP', 'BOTTOM', 'NONE'];
const TYPES = ['INNERCENTER', 'BEAD', 'INSHOULDER'];
const IMAGE_KINDS = { '.mim': 'mim', '.tif': 'tif', '.tiff': 'tif', '.png': 'png', '.bmp': 'bmp', '.jpg': 'jpg', '.jpeg': 'jpg', '.ini': 'ini' };
const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8', '.png': 'image/png', '.tif': 'image/tiff', '.tiff': 'image/tiff',
  '.f32': 'application/octet-stream', '.txt': 'text/plain; charset=utf-8', '.svg': 'image/svg+xml', '.ico': 'image/x-icon',
};
const CLI_TIMEOUT_MS = 30 * 60 * 1000;
const RUN_ID_RE = /^[A-Za-z0-9_\-.]+(\/[A-Za-z0-9_\-. ]+)?$/;

// ---------------------------------------------------------------- args / config
function parseArgs(argv) {
  const out = { roots: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) continue;
    const key = a.slice(2);
    const val = argv[i + 1];
    if (val === undefined) continue;
    if (key === 'root') out.roots.push(...val.split(';').map((s) => s.trim()).filter(Boolean));
    else out[key] = val;
    i++;
  }
  return out;
}
const ARGS = parseArgs(process.argv.slice(2));
// --allow-any-path lifts the /api/browse restriction to the configured roots (default: restricted)
const ALLOW_ANY_PATH = process.argv.includes('--allow-any-path');
const CONFIG = {
  port: parseInt(ARGS.port, 10) || DEFAULTS.port,
  host: ARGS.host || DEFAULTS.host,
  cli: ARGS.cli || DEFAULTS.cli,
  roots: ARGS.roots.length ? ARGS.roots : DEFAULTS.roots,
  ini: ARGS.ini || DEFAULTS.ini,
  fovproc: ARGS.fovproc || DEFAULTS.fovproc,
};

function resolveCli(value) {
  let v = String(value).trim();
  const m = /^node(?:\.exe)?\s+(.+)$/i.exec(v);
  if (m) v = m[1].trim();
  v = v.replace(/^"(.*)"$/, '$1');
  if (m || /\.js$/i.test(v)) {
    const script = path.resolve(ROOT, v);
    return { exe: process.execPath, pre: [script], display: `node ${script}`, exists: fs.existsSync(script) };
  }
  return { exe: v, pre: [], display: v, exists: fs.existsSync(v) };
}

// ---------------------------------------------------------------- ini parsing
function readTextFile(file) {
  const buf = fs.readFileSync(file);
  if (buf.length >= 2 && buf[0] === 0xff && buf[1] === 0xfe) return buf.toString('utf16le', 2);
  if (buf.length >= 2 && buf[0] === 0xfe && buf[1] === 0xff) return buf.swap16().toString('utf16le', 2);
  if (buf.length >= 3 && buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf) return buf.toString('utf8', 3);
  return buf.toString('utf8');
}
function parseIni(text) {
  const sections = {};
  let cur = null;
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith(';') || line.startsWith('#')) continue;
    const sec = /^\[(.+)\]$/.exec(line);
    if (sec) { cur = {}; sections[sec[1].trim().toUpperCase()] = cur; continue; }
    if (!cur) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    cur[line.slice(0, eq).trim().toUpperCase()] = line.slice(eq + 1).trim();
  }
  return sections;
}
function intList(s, n) {
  if (s == null) return null;
  const parts = String(s).split(',').map((v) => parseInt(v.trim(), 10));
  if (parts.length !== n || parts.some((v) => !Number.isFinite(v))) return null;
  return parts;
}
function loadPresets() {
  try {
    const ini = parseIni(readTextFile(CONFIG.ini));
    const presets = [];
    for (const [name, sec] of Object.entries(ini)) {
      const m = /^CAL(\d+)$/.exec(name);
      if (!m) continue;
      const cal = parseInt(m[1], 10);
      const patch = intList(sec['PATCHSIZE'], 2) || [15, 15];
      const overlap = sec['OVERLAP'] !== undefined ? parseFloat(sec['OVERLAP']) : 0.5;
      const stage = STAGES.includes((sec['STAGEPOSITION'] || '').toUpperCase()) ? sec['STAGEPOSITION'].toUpperCase() : 'AUTO';
      presets.push({
        name: sec['NAME'] || `CAL${cal}`, cal,
        type: TYPES.includes((sec['DEPTHPREPROCTYPE'] || '').toUpperCase()) ? sec['DEPTHPREPROCTYPE'].toUpperCase() : 'INSHOULDER',
        patch, overlap: Number.isFinite(overlap) ? overlap : 0.5,
        lower: Number.isFinite(parseFloat(sec['LOWER PERCENTAGE'])) ? parseFloat(sec['LOWER PERCENTAGE']) : 5,
        upper: Number.isFinite(parseFloat(sec['UPPER PERCENTAGE'])) ? parseFloat(sec['UPPER PERCENTAGE']) : 95,
        stage, roi: intList(sec['ROI'], 4) || [9999, 9999, 0, 0],
      });
    }
    presets.sort((a, b) => a.cal - b.cal);
    if (presets.length) return { presets, source: CONFIG.ini };
  } catch (e) { /* fall through to defaults */ }
  return { presets: DEFAULT_PRESETS, source: null };
}
function loadMapping() {
  try {
    const ini = parseIni(readTextFile(CONFIG.fovproc));
    const mapping = {};
    for (const [name, sec] of Object.entries(ini)) {
      if (!/^FOVPROC\d+$/.test(name)) continue;
      const req = parseInt(sec['REQUIREIMGIDX'], 10), res = parseInt(sec['RESULTIMGIDX'], 10), cal = parseInt(sec['PARAMIDX'], 10);
      if (!Number.isFinite(req) || !Number.isFinite(res)) continue;
      mapping[req] = { result: res, cal: Number.isFinite(cal) ? cal : null, name: sec['NAME'] || `FOV${req}` };
    }
    if (Object.keys(mapping).length) return { mapping, source: CONFIG.fovproc };
  } catch (e) { /* fall through */ }
  return { mapping: DEFAULT_MAPPING, source: null };
}

// ---------------------------------------------------------------- http helpers
function sendJson(res, status, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
  res.end(body);
}
function sendError(res, status, message, extra) {
  sendJson(res, status, Object.assign({ error: message }, extra || {}));
}
function readBody(req, limit = 4 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) { reject(new Error('request body too large')); req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}
async function readJson(req) {
  const buf = await readBody(req);
  if (!buf.length) return {};
  return JSON.parse(buf.toString('utf8'));
}
function serveFile(req, res, file, cacheControl) {
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) return sendError(res, 404, 'not found');
    const headers = {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Content-Length': st.size,
      'Cache-Control': cacheControl || 'no-cache',
    };
    res.writeHead(200, headers);
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).on('error', () => res.destroy()).pipe(res);
  });
}
function insideDir(dir, file) {
  const rel = path.relative(dir, file);
  return rel && !rel.startsWith('..') && !path.isAbsolute(rel);
}

// ---------------------------------------------------------------- run ids / queue
let runCounter = 0;
function newRunId(prefix) {
  const d = new Date();
  const p = (n, w = 2) => String(n).padStart(w, '0');
  const ts = `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
  runCounter = (runCounter + 1) % 1000;
  return `${prefix ? prefix + '_' : ''}${ts}_${p(runCounter, 3)}`;
}
let queue = Promise.resolve();
function enqueue(fn) {
  const p = queue.then(fn, fn);
  queue = p.catch(() => {});
  return p;
}
function runCli(args) {
  const cli = resolveCli(CONFIG.cli);
  const fullArgs = [...cli.pre, ...args];
  const t0 = Date.now();
  console.log(`[cli] ${cli.exe} ${fullArgs.map((a) => (/\s/.test(a) ? `"${a}"` : a)).join(' ')}`);
  return new Promise((resolve, reject) => {
    let child;
    try {
      child = spawn(cli.exe, fullArgs, { cwd: ROOT, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) { return reject(e); }
    let stdout = '', stderr = '';
    const timer = setTimeout(() => { try { child.kill(); } catch (e) { /* ignore */ } }, CLI_TIMEOUT_MS);
    child.stdout.on('data', (d) => { stdout += d.toString('utf8'); });
    child.stderr.on('data', (d) => { stderr += d.toString('utf8'); });
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      const durationMs = Date.now() - t0;
      console.log(`[cli] exit=${code} signal=${signal || ''} ${durationMs} ms`);
      resolve({ code: code === null ? -1 : code, signal, stdout, stderr, durationMs, cmd: [cli.exe, ...fullArgs] });
    });
  });
}
function cliErrorMessage(r) {
  const lines = r.stderr.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
  for (let i = lines.length - 1; i >= 0; i--) {
    try { const j = JSON.parse(lines[i]); if (j && typeof j === 'object' && j.error) return String(j.error); } catch (e) { /* not json */ }
  }
  return lines.join(' ') || r.stdout.trim() || (r.signal ? `CLI terminated (${r.signal})` : `CLI exit code ${r.code}`);
}
function httpStatusForExit(code) {
  if (code === 2) return 400;
  if (code === 3) return 404;
  if (code === 4) return 500;
  return 500;
}
function respondCliFailure(res, r, extra) {
  sendJson(res, httpStatusForExit(r.code), Object.assign({
    error: cliErrorMessage(r), exitCode: r.code, stderr: r.stderr, stdout: r.stdout, cmd: r.cmd, durationMs: r.durationMs,
  }, extra || {}));
}

// ---------------------------------------------------------------- params -> CLI args
function toInt(v, def) { const n = parseInt(v, 10); return Number.isFinite(n) ? n : def; }
function toNum(v, def) { const n = parseFloat(v); return Number.isFinite(n) ? n : def; }
function normalizeParams(raw) {
  const p = raw && typeof raw === 'object' ? raw : {};
  let patchW = 15, patchH = 15;
  if (Array.isArray(p.patch) && p.patch.length === 2) { patchW = toInt(p.patch[0], 15); patchH = toInt(p.patch[1], 15); }
  else if (typeof p.patch === 'string') { const l = intList(p.patch, 2); if (l) [patchW, patchH] = l; }
  if (p.patch_w !== undefined) patchW = toInt(p.patch_w, patchW);
  if (p.patch_h !== undefined) patchH = toInt(p.patch_h, patchH);
  let roi = null;
  const r = Array.isArray(p.roi) ? p.roi.map((v) => toInt(v, NaN)) : (typeof p.roi === 'string' ? intList(p.roi, 4) : null);
  if (r && r.length === 4 && r.every(Number.isFinite) && r[2] > r[0] && r[3] > r[1]) roi = r;
  const e = p.exp && typeof p.exp === 'object' ? p.exp : {};
  const flag = (a, b) => (a !== undefined ? a : b);
  const exp = {
    use_ini_pct: !!flag(e.use_ini_pct, p.exp_use_ini_pct),
    valid_pct: !!flag(e.valid_pct, p.exp_valid_pct),
    masked_median: !!flag(e.masked_median, p.exp_masked_median),
    null_value: toInt(flag(e.null_value, p.exp_null_value), -1),
    fill_holes: Math.max(0, toInt(flag(e.fill_holes, p.exp_fill_holes), 0)),
    stage_restore: !!flag(e.stage_restore, p.exp_stage_restore),
    abs_mm: Math.min(50, Math.max(0, toNum(flag(e.abs_mm, p.exp_abs_mm), 0))),
  };
  const stage = String(p.stage || 'AUTO').toUpperCase();
  const type = p.type ? String(p.type).toUpperCase() : null;
  const out = {
    type: TYPES.includes(type) ? type : null,
    patch_w: patchW, patch_h: patchH,
    overlap: toNum(p.overlap, 0.25),
    lower: toNum(p.lower, 5), upper: toNum(p.upper, 95),
    stage: STAGES.includes(stage) ? stage : 'AUTO',
    // INSHOULDER erosion kernel; null = CLI default (3, the site literal)
    break_kernel: (p.break_kernel !== undefined && p.break_kernel !== null && p.break_kernel !== '')
      ? Math.min(99, Math.max(0, toInt(p.break_kernel, 3))) : null,
    roi, exp,
    dump: p.dump === 'min' ? 'min' : 'all',
    preview: Math.max(1, toInt(p.preview, 8)),
    px_x: p.px_x !== undefined && p.px_x !== null && p.px_x !== '' ? toNum(p.px_x, null) : null,
    px_y: p.px_y !== undefined && p.px_y !== null && p.px_y !== '' ? toNum(p.px_y, null) : null,
    cal: p.cal !== undefined && p.cal !== null && p.cal !== '' ? toInt(p.cal, null) : null,
    override_all: !!p.override_all,
  };
  return out;
}
function paramArgs(p, includeCore) {
  const a = [];
  if (includeCore) {
    if (p.type) a.push('--type', p.type);
    a.push('--patch', `${p.patch_w},${p.patch_h}`);
    a.push('--overlap', String(p.overlap));
    a.push('--lower', String(p.lower), '--upper', String(p.upper));
    a.push('--stage', p.stage);
    if (p.roi) a.push('--roi', p.roi.join(','));
    if (p.break_kernel !== undefined && p.break_kernel !== null && p.break_kernel !== '') a.push('--break-kernel', String(p.break_kernel));
  }
  a.push('--exp-use-ini-pct', p.exp.use_ini_pct ? '1' : '0');
  a.push('--exp-valid-pct', p.exp.valid_pct ? '1' : '0');
  a.push('--exp-masked-median', p.exp.masked_median ? '1' : '0');
  a.push('--exp-null-value', String(p.exp.null_value));
  a.push('--exp-fill-holes', String(p.exp.fill_holes));
  a.push('--exp-stage-restore', p.exp.stage_restore ? '1' : '0');
  a.push('--exp-abs-mm', String(p.exp.abs_mm));
  a.push('--dump', p.dump, '--preview', String(p.preview));
  if (p.px_x !== null && p.px_y !== null) a.push('--px-x', String(p.px_x), '--px-y', String(p.px_y));
  return a;
}

// ---------------------------------------------------------------- run outputs
function fileKey(name) {
  const ext = path.extname(name).toLowerCase();
  const stem = name.slice(0, name.length - ext.length);
  if (name.toLowerCase() === 'stats.json') return 'stats';
  if (ext === '.png') return stem;
  if (ext === '.f32') return `${stem}_f32`;
  if (name.toLowerCase() === 'result_f32.tif') return 'result_f32';
  return `${stem}${ext.replace('.', '_')}`;
}
async function collectFiles(dir, urlBase) {
  const files = {};
  let names = [];
  try { names = await fsp.readdir(dir); } catch (e) { return files; }
  for (const name of names) {
    let st;
    try { st = await fsp.stat(path.join(dir, name)); } catch (e) { continue; }
    if (!st.isFile()) continue;
    files[fileKey(name)] = `${urlBase}/${encodeURIComponent(name)}`;
  }
  return files;
}
async function readStats(dir) {
  return JSON.parse(await fsp.readFile(path.join(dir, 'stats.json'), 'utf8'));
}
function runUrlBase(runId) { return '/runs/' + runId.split('/').map(encodeURIComponent).join('/'); }
function runDirOf(runId) { return path.join(RUNS_DIR, ...runId.split('/')); }

// ---------------------------------------------------------------- pixel inspector (f32 dumps + PNG decoder)
function decodePng(buf) {
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  if (buf.length < 8 || !buf.subarray(0, 8).equals(sig)) throw new Error('not a PNG file');
  let off = 8, width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idats = [];
  while (off + 8 <= buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('latin1', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9]; interlace = data[12];
    } else if (type === 'IDAT') idats.push(data);
    else if (type === 'IEND') break;
    off += 12 + len;
  }
  if (!width || !height) throw new Error('PNG without IHDR');
  if (interlace !== 0) throw new Error('interlaced PNG not supported');
  const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colorType];
  if (!channels || (bitDepth !== 8 && bitDepth !== 16)) throw new Error(`unsupported PNG format (colorType ${colorType}, bitDepth ${bitDepth})`);
  const bpp = channels * (bitDepth >> 3);
  const stride = width * bpp;
  const raw = zlib.inflateSync(Buffer.concat(idats));
  if (raw.length < (stride + 1) * height) throw new Error('PNG data truncated');
  const out = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const src = y * (stride + 1) + 1;
    const dst = y * stride;
    const up = dst - stride;
    switch (filter) {
      case 0:
        raw.copy(out, dst, src, src + stride);
        break;
      case 1:
        for (let i = 0; i < stride; i++) out[dst + i] = (raw[src + i] + (i >= bpp ? out[dst + i - bpp] : 0)) & 255;
        break;
      case 2:
        if (y === 0) raw.copy(out, dst, src, src + stride);
        else for (let i = 0; i < stride; i++) out[dst + i] = (raw[src + i] + out[up + i]) & 255;
        break;
      case 3:
        for (let i = 0; i < stride; i++) {
          const a = i >= bpp ? out[dst + i - bpp] : 0;
          const b = y > 0 ? out[up + i] : 0;
          out[dst + i] = (raw[src + i] + ((a + b) >> 1)) & 255;
        }
        break;
      case 4:
        for (let i = 0; i < stride; i++) {
          const a = i >= bpp ? out[dst + i - bpp] : 0;
          const b = y > 0 ? out[up + i] : 0;
          const c = (y > 0 && i >= bpp) ? out[up + i - bpp] : 0;
          const p = a + b - c;
          const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
          const pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          out[dst + i] = (raw[src + i] + pred) & 255;
        }
        break;
      default:
        throw new Error(`bad PNG filter ${filter} at row ${y}`);
    }
  }
  return { width, height, bitDepth, colorType, channels, bpp, stride, data: out };
}
function pngSample(im, x, y) {
  const o = y * im.stride + x * im.bpp;
  return im.bitDepth === 16 ? (im.data[o] << 8) | im.data[o + 1] : im.data[o];
}

class PixelSource {
  constructor(runId, dir, stats) {
    this.runId = runId; this.dir = dir;
    this.width = stats.input.width; this.height = stats.input.height;
    this.fds = {}; this.pngs = {};
  }
  f32(name, x, y) {
    if (!(name in this.fds)) {
      const f = path.join(this.dir, `${name}.f32`);
      let fd = null;
      try { if (fs.statSync(f).size >= this.width * this.height * 4) fd = fs.openSync(f, 'r'); } catch (e) { fd = null; }
      this.fds[name] = fd;
    }
    const fd = this.fds[name];
    if (fd === null) return null;
    const buf = Buffer.alloc(4);
    const n = fs.readSync(fd, buf, 0, 4, (y * this.width + x) * 4);
    return n === 4 ? buf.readFloatLE(0) : null;
  }
  png(name, x, y) {
    if (!(name in this.pngs)) {
      const f = path.join(this.dir, `${name}.png`);
      try { this.pngs[name] = decodePng(fs.readFileSync(f)); } catch (e) { this.pngs[name] = null; }
    }
    const im = this.pngs[name];
    if (!im || x >= im.width || y >= im.height) return null;
    return pngSample(im, x, y);
  }
  close() { for (const fd of Object.values(this.fds)) if (fd !== null) { try { fs.closeSync(fd); } catch (e) { /* ignore */ } } }
}
const pixelCache = new Map();
const PIXEL_CACHE_MAX = 6;
async function getPixelSource(runId) {
  if (pixelCache.has(runId)) {
    const v = pixelCache.get(runId);
    pixelCache.delete(runId); pixelCache.set(runId, v);
    return v;
  }
  const dir = runDirOf(runId);
  const stats = await readStats(dir);
  const src = new PixelSource(runId, dir, stats);
  pixelCache.set(runId, src);
  while (pixelCache.size > PIXEL_CACHE_MAX) {
    const [k, v] = pixelCache.entries().next().value;
    v.close(); pixelCache.delete(k);
  }
  return src;
}

// ---------------------------------------------------------------- API handlers
function handleConfig(res) {
  const cli = resolveCli(CONFIG.cli);
  const { presets, source: iniSource } = loadPresets();
  const { mapping, source: fovSource } = loadMapping();
  sendJson(res, 200, {
    cli: cli.display, cliExists: cli.exists,
    roots: CONFIG.roots.filter((r) => fs.existsSync(r)).length ? CONFIG.roots.filter((r) => fs.existsSync(r)) : CONFIG.roots,
    presets, mapping,
    ini: CONFIG.ini, iniLoaded: !!iniSource,
    fovproc: CONFIG.fovproc, fovprocLoaded: !!fovSource,
    runsDir: RUNS_DIR, uploadsDir: UPLOADS_DIR,
  });
}

async function handleBrowse(url, res) {
  const q = (url.searchParams.get('path') || '').trim();
  if (!q) {
    const roots = CONFIG.roots.filter((r) => fs.existsSync(r));
    return sendJson(res, 200, { path: '', parent: null, dirs: roots.length ? roots : CONFIG.roots, files: [] });
  }
  let dir = path.win32.resolve(q);
  if (/^[A-Za-z]:$/.test(dir)) dir += '\\';
  // Only folders under a configured root may be listed (pass --allow-any-path to lift the restriction).
  if (!ALLOW_ANY_PATH) {
    const norm = (s) => path.win32.resolve(s).replace(/[\\/]+$/, '').toLowerCase();
    const ok = CONFIG.roots.some((r) => { const rr = norm(r); const dd = norm(dir); return dd === rr || dd.startsWith(rr + '\\') || rr.endsWith(':') && dd.startsWith(rr + '\\'); });
    if (!ok) return sendError(res, 403, `허용된 루트 밖의 폴더입니다: ${dir}`);
  }
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch (e) {
    const status = e.code === 'ENOENT' || e.code === 'ENOTDIR' ? 404 : (e.code === 'EPERM' || e.code === 'EACCES' ? 403 : 500);
    return sendError(res, status, `\uD3F4\uB354\uB97C \uC5F4 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${dir} (${e.code || e.message})`);
  }
  const dirs = [];
  const files = [];
  await Promise.all(entries.map(async (ent) => {
    if (ent.isDirectory()) { dirs.push(ent.name); return; }
    if (!ent.isFile()) return;
    const kind = IMAGE_KINDS[path.extname(ent.name).toLowerCase()];
    if (!kind) return;
    let size = 0;
    try { size = (await fsp.stat(path.join(dir, ent.name))).size; } catch (e) { /* ignore */ }
    files.push({ name: ent.name, size, kind });
  }));
  const cmp = (a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' });
  dirs.sort(cmp);
  files.sort((a, b) => cmp(a.name, b.name));
  const parentRaw = path.win32.dirname(dir);
  const parent = parentRaw === dir ? '' : parentRaw;
  sendJson(res, 200, { path: dir, parent, dirs, files });
}

async function handleRun(req, res) {
  let body;
  try { body = await readJson(req); } catch (e) { return sendError(res, 400, `\uC694\uCCAD \uBCF8\uBB38 \uC624\uB958: ${e.message}`); }
  const input = String(body.input || '').trim();
  const ref = body.ref ? String(body.ref).trim() : '';
  if (!input) return sendError(res, 400, '\uC785\uB825 \uD30C\uC77C(input)\uC774 \uC9C0\uC815\uB418\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4.');
  if (!fs.existsSync(input)) return sendError(res, 404, `\uC785\uB825 \uD30C\uC77C\uC774 \uC5C6\uC2B5\uB2C8\uB2E4: ${input}`);
  if (ref && !fs.existsSync(ref)) return sendError(res, 404, `\uCC38\uC870 \uD30C\uC77C\uC774 \uC5C6\uC2B5\uB2C8\uB2E4: ${ref}`);
  const p = normalizeParams(body.params);
  if (!(p.patch_w >= 1 && p.patch_h >= 1)) return sendError(res, 400, 'PatchSize \uB294 1 \uC774\uC0C1\uC758 \uC815\uC218\uC5EC\uC57C \uD569\uB2C8\uB2E4.');
  if (!(p.overlap > 0 && p.overlap < 1)) return sendError(res, 400, 'Overlap \uC740 (0, 1) \uAC1C\uAD6C\uAC04\uC774\uC5B4\uC57C \uD569\uB2C8\uB2E4.');
  const cli = resolveCli(CONFIG.cli);
  if (!cli.exists) return sendError(res, 503, `CLI \uB97C \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${cli.display}`);

  const runId = newRunId('');
  const outDir = path.join(RUNS_DIR, runId);
  await fsp.mkdir(outDir, { recursive: true });
  const args = ['run', '--in', input, '--out', outDir];
  if (ref) args.push('--ref', ref);
  args.push(...paramArgs(p, true));

  let r;
  try { r = await enqueue(() => runCli(args)); } catch (e) {
    return sendError(res, 503, `CLI \uC2E4\uD589 \uC2E4\uD328: ${e.message}`, { cmd: [cli.exe, ...cli.pre, ...args] });
  }
  const meta = { runId, kind: 'run', input, ref: ref || null, params: p, cmd: r.cmd, exitCode: r.code, durationMs: r.durationMs, time: new Date().toISOString() };
  fs.writeFile(path.join(outDir, 'run.json'), JSON.stringify(meta, null, 1), () => {});
  if (r.stderr) fs.writeFile(path.join(outDir, 'cli_stderr.txt'), r.stderr, () => {});
  if (r.code !== 0) return respondCliFailure(res, r, { runId });
  let stats;
  try { stats = await readStats(outDir); } catch (e) {
    return sendError(res, 500, `stats.json \uC744 \uC77D\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${e.message}`, { runId, stdout: r.stdout, stderr: r.stderr });
  }
  pixelCache.delete(runId);
  sendJson(res, 200, {
    runId, outDir, stats, files: await collectFiles(outDir, runUrlBase(runId)),
    input, ref: ref || null, params: p, cli: { durationMs: r.durationMs, stdout: r.stdout, cmd: r.cmd },
  });
}

async function collectBatchItems(batchId, outDir) {
  const items = [];
  let names = [];
  try { names = await fsp.readdir(outDir, { withFileTypes: true }); } catch (e) { return items; }
  let summary = null;
  try { summary = JSON.parse(await fsp.readFile(path.join(outDir, 'batch.json'), 'utf8')); } catch (e) { summary = null; }
  const summaryList = Array.isArray(summary) ? summary : (summary && Array.isArray(summary.items) ? summary.items : []);
  for (const ent of names) {
    if (!ent.isDirectory()) continue;
    const m = /^(\d+)_(.*)$/.exec(ent.name);
    if (!m) continue;
    const sub = path.join(outDir, ent.name);
    const runId = `${batchId}/${ent.name}`;
    let stats = null;
    try { stats = await readStats(sub); } catch (e) { stats = null; }
    const extra = summaryList.find((s) => s && (String(s.imgIdx) === m[1] || String(s.img_idx) === m[1]) ) || null;
    items.push({
      imgIdx: parseInt(m[1], 10), name: m[2], runId, dir: sub, stats,
      files: await collectFiles(sub, runUrlBase(runId)),
      summary: extra,
      error: stats ? null : (extra && extra.error ? extra.error : 'stats.json \uC5C6\uC74C'),
    });
  }
  items.sort((a, b) => a.imgIdx - b.imgIdx);
  return { items, summary };
}

async function handleBatch(req, res) {
  let body;
  try { body = await readJson(req); } catch (e) { return sendError(res, 400, `\uC694\uCCAD \uBCF8\uBB38 \uC624\uB958: ${e.message}`); }
  const folder = String(body.folder || '').trim();
  if (!folder) return sendError(res, 400, '\uD0C0\uC774\uC5B4 \uD3F4\uB354(folder)\uAC00 \uC9C0\uC815\uB418\uC9C0 \uC54A\uC558\uC2B5\uB2C8\uB2E4.');
  let st = null;
  try { st = fs.statSync(folder); } catch (e) { st = null; }
  if (!st || !st.isDirectory()) return sendError(res, 404, `\uD3F4\uB354\uAC00 \uC5C6\uC2B5\uB2C8\uB2E4: ${folder}`);
  const fovproc = body.fovproc ? String(body.fovproc).trim() : (fs.existsSync(CONFIG.fovproc) ? CONFIG.fovproc : '');
  if (fovproc && !fs.existsSync(fovproc)) return sendError(res, 404, `FOVPROC.ini \uAC00 \uC5C6\uC2B5\uB2C8\uB2E4: ${fovproc}`);
  const p = normalizeParams(body.params);
  const cli = resolveCli(CONFIG.cli);
  if (!cli.exists) return sendError(res, 503, `CLI \uB97C \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${cli.display}`);

  const batchId = newRunId('batch');
  const outDir = path.join(RUNS_DIR, batchId);
  await fsp.mkdir(outDir, { recursive: true });
  const args = ['batch', '--folder', folder, '--out', outDir];
  if (fovproc) args.push('--fovproc', fovproc);
  if (fs.existsSync(CONFIG.ini)) args.push('--ini', CONFIG.ini);
  args.push(...paramArgs(p, p.override_all));

  let r;
  try { r = await enqueue(() => runCli(args)); } catch (e) {
    return sendError(res, 503, `CLI \uC2E4\uD589 \uC2E4\uD328: ${e.message}`, { cmd: [cli.exe, ...cli.pre, ...args] });
  }
  const meta = { runId: batchId, kind: 'batch', folder, fovproc: fovproc || null, params: p, cmd: r.cmd, exitCode: r.code, durationMs: r.durationMs, time: new Date().toISOString() };
  fs.writeFile(path.join(outDir, 'run.json'), JSON.stringify(meta, null, 1), () => {});
  if (r.stderr) fs.writeFile(path.join(outDir, 'cli_stderr.txt'), r.stderr, () => {});
  // exit 4 with a batch.json = some items failed; still return the good items with the error attached
  if (r.code !== 0 && !fs.existsSync(path.join(outDir, 'batch.json'))) return respondCliFailure(res, r, { batchId });
  const { items, summary } = await collectBatchItems(batchId, outDir);
  const cliError = r.code !== 0 ? { exitCode: r.code, stderr: r.stderr } : null;
  sendJson(res, 200, { batchId, outDir, folder, items, batch: summary, cliError, cli: { durationMs: r.durationMs, stdout: r.stdout, cmd: r.cmd } });
}

async function handlePixel(url, res) {
  const runId = (url.searchParams.get('runId') || '').trim();
  const x = parseInt(url.searchParams.get('x'), 10);
  const y = parseInt(url.searchParams.get('y'), 10);
  if (!RUN_ID_RE.test(runId) || runId.includes('..')) return sendError(res, 400, 'runId \uD615\uC2DD\uC774 \uC62C\uBC14\uB974\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4.');
  if (!Number.isFinite(x) || !Number.isFinite(y)) return sendError(res, 400, 'x, y \uB294 \uC815\uC218\uC5EC\uC57C \uD569\uB2C8\uB2E4.');
  let src;
  try { src = await getPixelSource(runId); } catch (e) { return sendError(res, 404, `\uC2E4\uD589 \uACB0\uACFC\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${runId}`); }
  const out = { runId, x, y, width: src.width, height: src.height, raw: null, basis: null, diff: null, result: null, ref: null };
  if (x >= 0 && y >= 0 && x < src.width && y < src.height) {
    try {
      out.raw = src.f32('raw', x, y);
      out.basis = src.f32('basis', x, y);
      out.diff = src.f32('diff', x, y);
      out.result = src.png('result', x, y);
      out.ref = src.png('ref', x, y);
    } catch (e) { out.error = e.message; }
  } else out.outside = true;
  for (const k of ['raw', 'basis', 'diff']) if (typeof out[k] === 'number' && !Number.isFinite(out[k])) out[k] = null;
  out.is_null = out.raw !== null && out.raw <= -900;
  sendJson(res, 200, out);
}

function sanitizeUploadName(name) {
  let base = path.basename(String(name || '').replace(/\\/g, '/')).trim();
  // keep Unicode (Korean) names; replace only characters Windows forbids in file names
  base = base.replace(/[<>:"|?*\x00-\x1f\\/]/g, '_').replace(/^\.+/, '');
  if (!base) base = 'upload.bin';
  return base;
}
async function handleUpload(req, url, res) {
  const name = sanitizeUploadName(url.searchParams.get('name'));
  await fsp.mkdir(UPLOADS_DIR, { recursive: true });
  const dest = path.join(UPLOADS_DIR, name);
  const tmp = `${dest}.part`;
  await new Promise((resolve, reject) => {
    const ws = fs.createWriteStream(tmp);
    req.on('error', reject);
    ws.on('error', reject);
    ws.on('finish', resolve);
    req.pipe(ws);
  });
  await fsp.rename(tmp, dest);
  const st = await fsp.stat(dest);
  sendJson(res, 200, { path: dest, name, size: st.size });
}

async function handleRunsList(res) {
  let names = [];
  try { names = await fsp.readdir(RUNS_DIR, { withFileTypes: true }); } catch (e) { names = []; }
  const runs = [];
  for (const ent of names) {
    if (!ent.isDirectory()) continue;
    const dir = path.join(RUNS_DIR, ent.name);
    let meta = null, stats = null;
    try { meta = JSON.parse(await fsp.readFile(path.join(dir, 'run.json'), 'utf8')); } catch (e) { meta = null; }
    const kind = meta ? meta.kind : (ent.name.startsWith('batch_') ? 'batch' : 'run');
    if (kind === 'run') { try { stats = await readStats(dir); } catch (e) { stats = null; } }
    let mtime = null;
    try { mtime = (await fsp.stat(dir)).mtime.toISOString(); } catch (e) { mtime = null; }
    runs.push({
      runId: ent.name, kind, time: meta ? meta.time : mtime,
      input: meta ? (meta.input || meta.folder || null) : (stats && stats.input ? stats.input.path : null),
      ok: kind === 'run' ? !!stats : (meta ? meta.exitCode === 0 : true),
      dll_identical: stats && stats.params_effective ? stats.params_effective.dll_identical !== false : null,
    });
  }
  runs.sort((a, b) => (b.runId > a.runId ? 1 : -1));
  sendJson(res, 200, { runs: runs.slice(0, 300) });
}
async function handleRunGet(url, res) {
  const runId = (url.searchParams.get('runId') || '').trim();
  if (!RUN_ID_RE.test(runId) || runId.includes('..')) return sendError(res, 400, 'runId \uD615\uC2DD\uC774 \uC62C\uBC14\uB974\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4.');
  const dir = runDirOf(runId);
  let meta = null;
  try { meta = JSON.parse(await fsp.readFile(path.join(dir, 'run.json'), 'utf8')); } catch (e) { meta = null; }
  if (meta && meta.kind === 'batch' && !runId.includes('/')) {
    const { items, summary } = await collectBatchItems(runId, dir);
    return sendJson(res, 200, { batchId: runId, outDir: dir, folder: meta.folder, items, batch: summary, meta });
  }
  let stats;
  try { stats = await readStats(dir); } catch (e) { return sendError(res, 404, `\uC2E4\uD589 \uACB0\uACFC\uB97C \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4: ${runId}`); }
  sendJson(res, 200, {
    runId, outDir: dir, stats, files: await collectFiles(dir, runUrlBase(runId)),
    input: meta ? meta.input : (stats.input ? stats.input.path : null), ref: meta ? meta.ref : (stats.ref ? stats.ref.path : null),
    params: meta ? meta.params : null, meta,
  });
}

// ---------------------------------------------------------------- router
async function route(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const p = url.pathname;
  const m = req.method;
  if (p === '/api/config' && m === 'GET') return handleConfig(res);
  if (p === '/api/browse' && m === 'GET') return handleBrowse(url, res);
  if (p === '/api/run' && m === 'POST') return handleRun(req, res);
  if (p === '/api/run' && m === 'GET') return handleRunGet(url, res);
  if (p === '/api/runs' && m === 'GET') return handleRunsList(res);
  if (p === '/api/batch' && m === 'POST') return handleBatch(req, res);
  if (p === '/api/pixel' && m === 'GET') return handlePixel(url, res);
  if (p === '/api/upload' && (m === 'PUT' || m === 'POST')) return handleUpload(req, url, res);
  if (p.startsWith('/api/')) return sendError(res, 404, `unknown api: ${m} ${p}`);
  if (m !== 'GET' && m !== 'HEAD') return sendError(res, 405, 'method not allowed');
  try { if (decodeURIComponent(p).includes('\0')) return sendError(res, 400, 'bad request'); } catch (e) { return sendError(res, 400, 'bad request'); }
  if (p.startsWith('/runs/')) {
    const rel = decodeURIComponent(p.slice('/runs/'.length));
    const file = path.join(RUNS_DIR, rel);
    if (!insideDir(RUNS_DIR, file)) return sendError(res, 403, 'forbidden');
    return serveFile(req, res, file, 'public, max-age=3600');
  }
  let rel = decodeURIComponent(p);
  if (rel === '/' || rel === '') rel = '/index.html';
  if (rel === '/favicon.ico') { res.writeHead(204); return res.end(); }
  const file = path.join(PUBLIC_DIR, rel);
  if (!insideDir(PUBLIC_DIR, file)) return sendError(res, 403, 'forbidden');
  return serveFile(req, res, file, 'no-cache');
}

const server = http.createServer((req, res) => {
  route(req, res).catch((e) => {
    console.error(`[error] ${req.method} ${req.url}: ${e.stack || e}`);
    if (!res.headersSent) sendError(res, 500, e.message || String(e));
    else res.destroy();
  });
});
server.requestTimeout = 0;
server.headersTimeout = 120000;
server.keepAliveTimeout = 30000;

fs.mkdirSync(RUNS_DIR, { recursive: true });
fs.mkdirSync(UPLOADS_DIR, { recursive: true });
server.listen(CONFIG.port, CONFIG.host, () => {
  const cli = resolveCli(CONFIG.cli);
  console.log(`DepthPreprocSim UI  http://${CONFIG.host}:${CONFIG.port}/`);
  console.log(`  cli     : ${cli.display}${cli.exists ? '' : '  (NOT FOUND)'}`);
  console.log(`  ini     : ${CONFIG.ini}${fs.existsSync(CONFIG.ini) ? '' : '  (not found, using built-in presets)'}`);
  console.log(`  fovproc : ${CONFIG.fovproc}${fs.existsSync(CONFIG.fovproc) ? '' : '  (not found, using built-in mapping)'}`);
  console.log(`  roots   : ${CONFIG.roots.join(' | ')}`);
  console.log(`  runs    : ${RUNS_DIR}`);
});
server.on('error', (e) => {
  console.error(`[fatal] ${e.code === 'EADDRINUSE' ? `port ${CONFIG.port} already in use` : e.message}`);
  process.exit(1);
});
process.on('SIGINT', () => { server.close(); process.exit(0); });
process.on('SIGTERM', () => { server.close(); process.exit(0); });
