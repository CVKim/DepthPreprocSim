'use strict';
/*
 * UI smoke test (DESIGN.md §5 T6). Starts ui/server.js on a spare port, exercises every API,
 * then kills the server. Works against the fake CLI or the real depth_sim.exe.
 *
 *   node ui/smoke_test.js [--cli "node ui/fake_cli.js"] [--port 8799] [--input <mim>] [--ref <mim>] [--batch-folder <dir>] [--no-batch]
 */
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const args = {};
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '--no-batch') { args.noBatch = true; continue; }
  if (a.startsWith('--')) { args[a.slice(2)] = process.argv[++i]; }
}
const PORT = parseInt(args.port, 10) || 8799;
const CLI = args.cli || 'node ui/fake_cli.js';
const TIRE1 = 'H:\\000. PJT\\01. HankookTire\\03_\uC774\uBBF8\uC9C0\\PC3\\1639390184_2000026091709423060';
const TIRE2 = 'H:\\000. PJT\\01. HankookTire\\03_\uC774\uBBF8\uC9C0\\PC3\\2000026091715281105';
const INPUT = args.input || path.join(TIRE1, '7_SWU_Sh_R_D.mim');
const REF = args.ref || path.join(TIRE1, '12_SWU_Sh_R_D_Proc.mim');
const BATCH_FOLDER = args['batch-folder'] || TIRE2;
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
function check(cond, label, detail) {
  if (cond) console.log(`  [ok]   ${label}${detail !== undefined ? `  -> ${detail}` : ''}`);
  else { failures++; console.log(`  [FAIL] ${label}${detail !== undefined ? `  -> ${detail}` : ''}`); }
}
async function getJson(url, opts) {
  const res = await fetch(url, opts);
  let body = null;
  try { body = await res.json(); } catch (e) { body = null; }
  return { status: res.status, body, headers: res.headers };
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  const serverArgs = [path.join(ROOT, 'ui', 'server.js'), '--port', String(PORT), '--cli', CLI];
  if (args.runs) serverArgs.push('--runs', args.runs);
  console.log(`server: node ${serverArgs.slice(1).map((a) => (/\s/.test(a) ? `"${a}"` : a)).join(' ')}`);
  const server = spawn(process.execPath, serverArgs, { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '';
  server.stdout.on('data', (d) => { log += d; });
  server.stderr.on('data', (d) => { log += d; });
  try {
    let up = false;
    for (let i = 0; i < 50 && !up; i++) { try { const r = await fetch(`${BASE}/api/config`); up = r.ok; } catch (e) { await sleep(100); } }
    check(up, 'server started');
    if (!up) throw new Error(`server did not start:\n${log}`);

    // GET /
    const home = await fetch(`${BASE}/`);
    const html = await home.text();
    check(home.status === 200 && /<title>DepthPreprocSim<\/title>/.test(html), 'GET / serves index.html', `${home.status}, ${html.length} bytes`);
    const js = await fetch(`${BASE}/app.js`); const css = await fetch(`${BASE}/style.css`);
    check(js.status === 200 && css.status === 200, 'GET /app.js, /style.css', `${js.headers.get('content-type')} / ${css.headers.get('content-type')}`);

    // /api/config
    const cfg = await getJson(`${BASE}/api/config`);
    check(cfg.status === 200 && cfg.body && Array.isArray(cfg.body.roots) && Array.isArray(cfg.body.presets) && cfg.body.mapping && typeof cfg.body.cli === 'string', '/api/config shape {cli, roots[], presets[], mapping}');
    check(cfg.body.presets.length >= 3 && cfg.body.presets.every((p) => 'name' in p && 'cal' in p && 'type' in p && Array.isArray(p.patch) && 'overlap' in p && 'lower' in p && 'upper' in p && 'stage' in p && Array.isArray(p.roi)), 'presets fields', JSON.stringify(cfg.body.presets.map((p) => [p.cal, p.name, p.patch.join('x'), p.overlap])));
    check(cfg.body.mapping['7'] && cfg.body.mapping['7'].result === 12 && cfg.body.mapping['7'].cal === 3, 'mapping 7 -> 12 / CAL3', JSON.stringify(cfg.body.mapping['7']));
    check(cfg.body.cliExists === true, 'CLI exists', cfg.body.cli);

    // /api/browse
    const rootsB = await getJson(`${BASE}/api/browse`);
    check(rootsB.status === 200 && Array.isArray(rootsB.body.dirs) && rootsB.body.dirs.length > 0, '/api/browse (roots)', JSON.stringify(rootsB.body.dirs));
    const dirB = await getJson(`${BASE}/api/browse?path=${encodeURIComponent(path.dirname(INPUT))}`);
    check(dirB.status === 200 && Array.isArray(dirB.body.files) && dirB.body.files.some((f) => f.name === path.basename(INPUT) && f.kind === 'mim' && f.size > 0), '/api/browse tire folder lists input .mim', `${dirB.body.files ? dirB.body.files.length : 0} files, ${dirB.body.dirs ? dirB.body.dirs.length : 0} dirs`);
    const badB = await getJson(`${BASE}/api/browse?path=${encodeURIComponent('E:\\__does_not_exist__')}`);
    check(badB.status === 404 && badB.body.error, '/api/browse missing folder -> 404 + error');

    // /api/run
    const params = { type: 'INSHOULDER', patch_w: 15, patch_h: 15, overlap: 0.25, stage: 'AUTO', roi: [9999, 9999, 0, 0], lower: 5, upper: 95, exp: { use_ini_pct: false, valid_pct: false, masked_median: false, null_value: -1, fill_holes: 0 }, dump: 'all', preview: 8 };
    const t0 = Date.now();
    const run = await getJson(`${BASE}/api/run`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ input: INPUT, ref: fs.existsSync(REF) ? REF : null, params }) });
    const rb = run.body || {};
    check(run.status === 200 && rb.runId && rb.stats && rb.files, '/api/run 200 {runId, outDir, stats, files}', `${rb.runId} in ${Date.now() - t0} ms${rb.error ? ` error=${rb.error}` : ''}`);
    if (run.status === 200) {
      const s = rb.stats;
      check(s.input && s.input.width > 0 && s.params_effective && s.null && s.output && s.diff_hist && s.timing_ms, 'stats.json top-level keys', `${s.input.width}x${s.input.height}, null ${s.null.pct}%, black ${s.output.black_pct}%, dll_identical=${s.params_effective.dll_identical}`);
      check(typeof rb.files.result === 'string' && rb.files.result.startsWith('/runs/'), 'files.result url', rb.files.result);
      for (const k of ['raw_vis', 'null_mask', 'stage_removed_mask', 'scaled', 'basis', 'diff', 'clipped', 'stats', 'raw_f32', 'basis_f32', 'diff_f32', 'preview_result']) check(k in rb.files, `files.${k} present`, rb.files[k]);
      if (s.ref) { check(s.ref.exact_pct >= 0 && 'black_mask_iou' in s.ref && 'ref' in rb.files && 'ref_diff' in rb.files, 'ref stats + ref.png/ref_diff.png', `exact ${s.ref.exact_pct}%, within1 ${s.ref.within1_pct}%, IoU ${s.ref.black_mask_iou}`); }
      const png = await fetch(`${BASE}${rb.files.result}`);
      const pngBuf = Buffer.from(await png.arrayBuffer());
      check(png.status === 200 && png.headers.get('content-type') === 'image/png' && pngBuf[0] === 0x89 && pngBuf[1] === 0x50, 'GET /runs/<id>/result.png', `${pngBuf.length} bytes`);
      const trav = await fetch(`${BASE}/runs/../DESIGN.md`);
      check(trav.status === 403 || trav.status === 404, 'GET /runs/../ traversal blocked', trav.status);

      // /api/pixel
      const x = Math.floor(s.input.width / 2), y = Math.floor((s.valid_rows.first + s.valid_rows.last) / 2);
      const px = await getJson(`${BASE}/api/pixel?runId=${encodeURIComponent(rb.runId)}&x=${x}&y=${y}`);
      check(px.status === 200 && px.body && ['raw', 'basis', 'diff', 'result', 'ref'].every((k) => k in px.body), '/api/pixel shape', JSON.stringify(px.body));
      check(typeof px.body.raw === 'number' && typeof px.body.basis === 'number' && typeof px.body.diff === 'number' && typeof px.body.result === 'number', '/api/pixel values from f32 + PNG decoder are numbers');
      if (s.ref) check(typeof px.body.ref === 'number', '/api/pixel ref value decoded', px.body.ref);
      const pxOut = await getJson(`${BASE}/api/pixel?runId=${encodeURIComponent(rb.runId)}&x=999999&y=0`);
      check(pxOut.status === 200 && pxOut.body.raw === null && pxOut.body.outside === true, '/api/pixel outside -> nulls');
      const pxBad = await getJson(`${BASE}/api/pixel?runId=..%2F..%2Fx&x=0&y=0`);
      check(pxBad.status === 400, '/api/pixel bad runId -> 400');
      // Cross-check the pixel endpoint against the raw.f32 file on disk.
      const rawFile = path.join(rb.outDir, 'raw.f32');
      if (fs.existsSync(rawFile)) {
        const fd = fs.openSync(rawFile, 'r'); const b = Buffer.alloc(4); fs.readSync(fd, b, 0, 4, (y * s.input.width + x) * 4); fs.closeSync(fd);
        check(Math.abs(b.readFloatLE(0) - px.body.raw) < 1e-6, 'pixel raw equals raw.f32 on disk', `${b.readFloatLE(0)} vs ${px.body.raw}`);
      }
      // GET /api/run?runId (reload) and /api/runs
      const reload = await getJson(`${BASE}/api/run?runId=${encodeURIComponent(rb.runId)}`);
      check(reload.status === 200 && reload.body.runId === rb.runId && reload.body.files.result, '/api/run?runId reload');
      const list = await getJson(`${BASE}/api/runs`);
      check(list.status === 200 && list.body.runs.some((r) => r.runId === rb.runId), '/api/runs lists the run');
    }

    // error paths
    const missing = await getJson(`${BASE}/api/run`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ input: 'E:\\__nope__.mim', params }) });
    check(missing.status === 404 && missing.body.error, '/api/run missing input -> 404', missing.body.error);
    const badPatch = await getJson(`${BASE}/api/run`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ input: INPUT, params: Object.assign({}, params, { patch_w: 0 }) }) });
    check(badPatch.status === 400 && badPatch.body.error, '/api/run bad patch -> 400', badPatch.body.error);
    const minRun = await getJson(`${BASE}/api/run`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ input: INPUT, params: Object.assign({}, params, { dump: 'min', exp: Object.assign({}, params.exp, { valid_pct: true }) }) }) });
    check(minRun.status === 200 && minRun.body.stats.params_effective.dll_identical === false && !('raw_f32' in minRun.body.files), '/api/run dump=min + exp flag -> dll_identical=false, no f32', Object.keys(minRun.body.files || {}).join(','));
    if (minRun.status === 200) {
      const pxMin = await getJson(`${BASE}/api/pixel?runId=${encodeURIComponent(minRun.body.runId)}&x=10&y=${minRun.body.stats.valid_rows.first}`);
      check(pxMin.status === 200 && pxMin.body.raw === null && typeof pxMin.body.result === 'number', '/api/pixel without dumps -> raw null, result from PNG', JSON.stringify(pxMin.body));
    }

    // /api/upload
    const payload = Buffer.from('II*\u0000' + 'x'.repeat(60), 'latin1');
    const upl = await getJson(`${BASE}/api/upload?name=${encodeURIComponent('smoke \uD55C\uAE00 test.mim')}`, { method: 'PUT', body: payload });
    check(upl.status === 200 && upl.body.path && fs.existsSync(upl.body.path) && fs.statSync(upl.body.path).size === payload.length, '/api/upload writes file', upl.body.path);
    if (upl.body && upl.body.path) fs.unlinkSync(upl.body.path);

    // /api/batch
    if (!args.noBatch && fs.existsSync(BATCH_FOLDER)) {
      const t1 = Date.now();
      const batch = await getJson(`${BASE}/api/batch`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ folder: BATCH_FOLDER, params, fovproc: null }) });
      const bb = batch.body || {};
      check(batch.status === 200 && bb.batchId && Array.isArray(bb.items), '/api/batch 200 {batchId, items[]}', `${bb.batchId}, ${bb.items ? bb.items.length : 0} items in ${Date.now() - t1} ms${bb.error ? ` error=${bb.error}` : ''}`);
      if (batch.status === 200) {
        check(bb.items.length === 8, 'batch has 8 items', bb.items.map((i) => i.imgIdx).join(','));
        check(bb.items.every((i) => 'imgIdx' in i && 'name' in i && i.stats && i.files && i.files.result), 'items have imgIdx/name/stats/files');
        const it = bb.items.find((i) => i.imgIdx === 7) || bb.items[0];
        check(it && it.stats.ref && it.stats.ref.exact_pct >= 0, 'batch item auto-compared with _Proc', it ? `idx ${it.imgIdx} exact ${it.stats.ref && it.stats.ref.exact_pct}%` : '');
        const pxb = await getJson(`${BASE}/api/pixel?runId=${encodeURIComponent(it.runId)}&x=5&y=${it.stats.valid_rows.first}`);
        check(pxb.status === 200 && 'raw' in pxb.body, '/api/pixel works for batch item runId', it.runId);
        const img = await fetch(`${BASE}${it.files.result}`);
        check(img.status === 200, 'GET batch item result.png', it.files.result);
      }
      const badBatch = await getJson(`${BASE}/api/batch`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ folder: 'E:\\__nope__', params }) });
      check(badBatch.status === 404, '/api/batch missing folder -> 404');
    } else console.log('  [skip] batch (folder missing or --no-batch)');

    const unknown = await getJson(`${BASE}/api/unknown`);
    check(unknown.status === 404, 'unknown api -> 404');
  } catch (e) {
    failures++;
    console.log(`  [FAIL] exception: ${e.stack || e}`);
  } finally {
    server.kill();
    await sleep(200);
    if (server.exitCode === null) { try { process.kill(server.pid); } catch (e) { /* ignore */ } }
    console.log(`server stopped (exitCode=${server.exitCode}, killed=${server.killed})`);
  }
  console.log(failures ? `SMOKE TEST FAILED: ${failures} check(s)` : 'SMOKE TEST PASSED');
  process.exit(failures ? 1 : 0);
}
main();
