(() => {
'use strict';
const $ = (id) => document.getElementById(id);

const LAYERS = [
  { key: 'raw_vis', label: 'raw (null=빨강)', def: true },
  { key: 'null_mask', label: 'null 마스크' },
  { key: 'stage_removed_mask', label: '지그 제거 마스크' },
  { key: 'scaled', label: 'scaled (16U)' },
  { key: 'basis', label: '기준면 basis' },
  { key: 'diff', label: 'diff' },
  { key: 'clipped', label: 'clipped' },
  { key: 'result', label: '결과 result', def: true },
  { key: 'ref', label: '참조 ref', def: true },
  { key: 'ref_diff', label: '|result − ref| × 8', def: true },
];
const LS_KEY = 'depthPreprocSim.form.v1';

const state = {
  config: null,
  slots: { A: null, B: null },
  showing: 'B',
  pinned: false,
  view: { scale: 1, tx: 0, ty: 0, fitted: true },
  panels: [],
  hover: null,
  drag: null,
  refAuto: true,
  busy: false,
  batch: null,
  redrawPending: false,
};

// ------------------------------------------------------------------ helpers
function fmt(v, d = 2) { return (v === null || v === undefined || Number.isNaN(v)) ? '–' : (typeof v === 'number' ? v.toFixed(d) : String(v)); }
function fmtInt(v) { return (v === null || v === undefined) ? '–' : Number(v).toLocaleString('ko-KR'); }
function fmtPct(v, d = 2) { return (v === null || v === undefined) ? '–' : `${Number(v).toFixed(d)} %`; }
function fmtBytes(n) { if (n < 1024) return `${n} B`; if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`; return `${(n / 1048576).toFixed(1)} MB`; }
function esc(s) { return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function dirname(p) { const i = Math.max(p.lastIndexOf('\\'), p.lastIndexOf('/')); return i < 0 ? '' : p.slice(0, i); }
function basename(p) { const i = Math.max(p.lastIndexOf('\\'), p.lastIndexOf('/')); return p.slice(i + 1); }
function joinPath(dir, name) { if (!dir) return name; return /[\\/]$/.test(dir) ? dir + name : `${dir}\\${name}`; }
function setStatus(el, text, cls) { el.textContent = text || ''; el.className = `status${cls ? ' ' + cls : ''}`; }
function currentRun() { return state.slots[state.showing] || null; }
async function api(url, opts) {
  const res = await fetch(url, opts);
  let body = null;
  try { body = await res.json(); } catch (e) { body = null; }
  if (!res.ok) throw new Error((body && body.error) || `${res.status} ${res.statusText}`);
  return body;
}
function postJson(url, obj) { return api(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(obj) }); }
function elapsedTimer(el, label) {
  const t0 = performance.now();
  const id = setInterval(() => setStatus(el, `${label} ${((performance.now() - t0) / 1000).toFixed(1)} 초 경과`, 'busy'), 200);
  setStatus(el, `${label} 0.0 초 경과`, 'busy');
  return () => { clearInterval(id); return (performance.now() - t0) / 1000; };
}

// ------------------------------------------------------------------ form <-> params
function readForm() {
  return {
    inputPath: $('inputPath').value, refPath: $('refPath').value, preset: $('preset').value, type: $('pType').value,
    patchW: $('patchW').value, patchH: $('patchH').value, overlap: $('overlap').value, stage: $('stage').value,
    breakKernel: $('breakKernel').value,
    roi: ['roiX1', 'roiY1', 'roiX2', 'roiY2'].map((id) => $(id).value), lower: $('lower').value, upper: $('upper').value,
    expUseIniPct: $('expUseIniPct').checked, expValidPct: $('expValidPct').checked, expMaskedMedian: $('expMaskedMedian').checked,
    expNullValue: $('expNullValue').value, expFillHoles: $('expFillHoles').value,
    dumpMode: $('dumpMode').value, previewN: $('previewN').value, pxX: $('pxX').value, pxY: $('pxY').value,
    layers: LAYERS.filter((L) => { const cb = $(`layer_${L.key}`); return cb && cb.checked; }).map((L) => L.key),
    cols: $('cols').value, batchFolder: $('batchFolder').value, batchFovproc: $('batchFovproc').value, batchOverride: $('batchOverride').checked,
    histLog: $('histLog').checked,
  };
}
function writeForm(f) {
  if (!f) return;
  const set = (id, v) => { if (v !== undefined && v !== null && $(id)) $(id).value = v; };
  const chk = (id, v) => { if (v !== undefined && $(id)) $(id).checked = !!v; };
  set('inputPath', f.inputPath); set('refPath', f.refPath); set('pType', f.type);
  set('patchW', f.patchW); set('patchH', f.patchH); set('overlap', f.overlap); set('stage', f.stage);
  set('breakKernel', f.breakKernel);
  if (Array.isArray(f.roi)) ['roiX1', 'roiY1', 'roiX2', 'roiY2'].forEach((id, i) => set(id, f.roi[i]));
  set('lower', f.lower); set('upper', f.upper);
  chk('expUseIniPct', f.expUseIniPct); chk('expValidPct', f.expValidPct); chk('expMaskedMedian', f.expMaskedMedian);
  set('expNullValue', f.expNullValue); set('expFillHoles', f.expFillHoles);
  set('dumpMode', f.dumpMode); set('previewN', f.previewN); set('pxX', f.pxX); set('pxY', f.pxY);
  if (Array.isArray(f.layers)) LAYERS.forEach((L) => { const cb = $(`layer_${L.key}`); if (cb) cb.checked = f.layers.includes(L.key); });
  set('cols', f.cols); set('batchFolder', f.batchFolder); set('batchFovproc', f.batchFovproc); chk('batchOverride', f.batchOverride);
  chk('histLog', f.histLog);
  if (f.preset !== undefined) $('preset').value = f.preset;
}
let saveTimer = null;
function saveForm() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { try { localStorage.setItem(LS_KEY, JSON.stringify(readForm())); } catch (e) { /* ignore */ } }, 150);
}
function loadForm() { try { return JSON.parse(localStorage.getItem(LS_KEY) || 'null'); } catch (e) { return null; } }

function gatherParams() {
  const num = (id, def) => { const v = parseFloat($(id).value); return Number.isFinite(v) ? v : def; };
  const int = (id, def) => { const v = parseInt($(id).value, 10); return Number.isFinite(v) ? v : def; };
  const presetOpt = $('preset').selectedOptions[0];
  return {
    cal: presetOpt && presetOpt.dataset.cal ? parseInt(presetOpt.dataset.cal, 10) : null,
    type: $('pType').value,
    patch_w: int('patchW', 15), patch_h: int('patchH', 15), overlap: num('overlap', 0.25), stage: $('stage').value,
    break_kernel: int('breakKernel', 3),
    roi: [int('roiX1', 9999), int('roiY1', 9999), int('roiX2', 0), int('roiY2', 0)],
    lower: num('lower', 5), upper: num('upper', 95),
    exp: {
      use_ini_pct: $('expUseIniPct').checked, valid_pct: $('expValidPct').checked, masked_median: $('expMaskedMedian').checked,
      null_value: int('expNullValue', -1), fill_holes: int('expFillHoles', 0),
      stage_restore: $('expStageRestore').checked, abs_mm: num('expAbsMm', 0),
    },
    dump: $('dumpMode').value, preview: int('previewN', 8),
    px_x: $('pxX').value === '' ? null : num('pxX', null), px_y: $('pxY').value === '' ? null : num('pxY', null),
  };
}
function updateDerived() {
  const pw = parseInt($('patchW').value, 10) || 0, ph = parseInt($('patchH').value, 10) || 0, ov = parseFloat($('overlap').value);
  const step = (p) => Math.max(1, Math.trunc(p * (1 - ov)));
  $('stepInfo').value = (pw > 0 && ph > 0 && ov > 0 && ov < 1) ? `${step(pw)} × ${step(ph)}` : '(유효하지 않은 값)';
  const r = ['roiX1', 'roiY1', 'roiX2', 'roiY2'].map((id) => parseInt($(id).value, 10));
  $('roiHint').textContent = (r[2] > r[0] && r[3] > r[1]) ? `ROI 사용: (${r[0]}, ${r[1]}) – (${r[2]}, ${r[3]}), ${r[2] - r[0]} × ${r[3] - r[1]} px` : 'ROI 미사용 (전체 이미지 처리)';
  const anyExp = $('expUseIniPct').checked || $('expValidPct').checked || $('expMaskedMedian').checked || parseInt($('expNullValue').value, 10) !== -1 || parseInt($('expFillHoles').value, 10) > 0;
  $('btnRun').textContent = anyExp ? '실행 (실험 옵션)' : '실행';
}
function applyPreset(preset) {
  if (!preset) return;
  $('pType').value = preset.type; $('patchW').value = preset.patch[0]; $('patchH').value = preset.patch[1];
  $('overlap').value = preset.overlap; $('stage').value = preset.stage || 'AUTO';
  ['roiX1', 'roiY1', 'roiX2', 'roiY2'].forEach((id, i) => { $(id).value = preset.roi ? preset.roi[i] : [9999, 9999, 0, 0][i]; });
  $('lower').value = preset.lower; $('upper').value = preset.upper;
  updateDerived(); saveForm();
}
function markCustomPreset() {
  const sel = $('preset');
  const opt = sel.selectedOptions[0];
  if (!opt || !opt.dataset.cal || !state.config) return;
  const p = state.config.presets.find((x) => String(x.cal) === opt.dataset.cal);
  if (!p) return;
  const same = $('pType').value === p.type && +$('patchW').value === p.patch[0] && +$('patchH').value === p.patch[1] && +$('overlap').value === p.overlap && $('stage').value === (p.stage || 'AUTO');
  if (!same) sel.value = '';
}

// ------------------------------------------------------------------ config / presets / mapping
async function loadConfig() {
  const cfg = await api('/api/config');
  state.config = cfg;
  const sel = $('preset');
  sel.innerHTML = '<option value="">(사용자 지정)</option>';
  for (const p of cfg.presets) {
    const o = document.createElement('option');
    o.value = String(p.cal); o.dataset.cal = String(p.cal);
    o.textContent = `CAL${String(p.cal).padStart(4, '0')} ${p.name} — ${p.patch[0]}×${p.patch[1]} / ${p.overlap} / ${p.type}`;
    sel.appendChild(o);
  }
  const cs = $('cliStatus');
  cs.textContent = cfg.cliExists ? `CLI: ${basename(cfg.cli)}` : 'CLI 없음 — build\\Release\\depth_sim.exe 를 빌드하거나 --cli 를 지정하십시오';
  cs.className = `badge ${cfg.cliExists ? 'ok' : 'err'}`;
  cs.title = cfg.cli;
  if (!$('batchFovproc').value) $('batchFovproc').value = cfg.fovproc || '';
  $('runsDirHint').textContent = `실행 결과 폴더: ${cfg.runsDir}`;
  if (!cfg.iniLoaded) $('preset').title = 'ini 를 읽지 못해 내장 프리셋을 사용합니다';
}
function mappingFor(inputPath) {
  const m = /^(\d+)_(.+)$/.exec(basename(inputPath));
  if (!m || !state.config) return null;
  const map = state.config.mapping[m[1]];
  if (!map) return null;
  const rest = m[2];
  const ext = /\.[^.]+$/.exec(rest);
  const stem = ext ? rest.slice(0, -ext[0].length) : rest;
  return { idx: parseInt(m[1], 10), map, refName: `${map.result}_${stem}_Proc${ext ? ext[0] : '.mim'}` };
}
async function guessRef(force) {
  const input = $('inputPath').value.trim();
  const info = mappingFor(input);
  const hint = $('refHint');
  if (!info) { hint.textContent = '입력 이름에서 FOVPROC 매핑 인덱스를 찾지 못했습니다. 참조는 직접 지정하십시오.'; return; }
  if (info.map.cal) {
    const opt = [...$('preset').options].find((o) => o.dataset.cal === String(info.map.cal));
    if (opt) { $('preset').value = opt.value; applyPreset(state.config.presets.find((p) => p.cal === info.map.cal)); }
  }
  if (!force && !state.refAuto && $('refPath').value.trim()) return;
  try {
    const dir = dirname(input);
    const j = await api(`/api/browse?path=${encodeURIComponent(dir)}`);
    const found = j.files.find((f) => f.name.toLowerCase() === info.refName.toLowerCase());
    if (found) { $('refPath').value = joinPath(j.path, found.name); state.refAuto = true; hint.textContent = `자동 추정: ${info.idx} → ${info.map.result} (${info.map.name}, CAL${info.map.cal})`; }
    else { hint.textContent = `같은 폴더에 ${info.refName} 이 없습니다. 참조 없이 실행하거나 직접 지정하십시오.`; if (state.refAuto) $('refPath').value = ''; }
  } catch (e) { hint.textContent = `참조 추정 실패: ${e.message}`; }
  saveForm();
}

// ------------------------------------------------------------------ run objects
function makeRun(resp, extra) {
  return Object.assign({ runId: resp.runId, outDir: resp.outDir, stats: resp.stats, files: resp.files || {}, images: {}, input: resp.input, ref: resp.ref, params: resp.params, kind: 'run' }, extra || {});
}
function placeRun(run) {
  const prev = currentRun();
  const sameDims = !!prev && prev.stats.input.width === run.stats.input.width && prev.stats.input.height === run.stats.input.height;
  if (state.pinned && state.slots.A) { state.slots.B = run; state.showing = 'B'; }
  else { state.slots.A = null; state.slots.B = run; state.showing = 'B'; state.pinned = false; }
  // Same geometry (A/B compare, parameter sweep on one image): keep the current zoom/pan. New geometry: fit.
  if (!sameDims) state.view.fitted = true;
  state.hover = null;
  clearInspector();
  showCurrent();
}
function showCurrent() {
  const run = currentRun();
  updateAbUi();
  rebuildPanels();
  renderStats(run);
  drawHist(run);
  drawRowBins(run);
  const identical = !run || !run.stats || !run.stats.params_effective || run.stats.params_effective.dll_identical !== false;
  $('dllBadge').classList.toggle('hidden', identical);
  $('statsWarn').classList.toggle('hidden', identical);
  $('runInfo').textContent = run ? `${run.runId} · ${run.stats.input.width}×${run.stats.input.height} · ${basename(run.stats.input.path || run.input || '')}` : '';
  $('runInfo').title = run ? (run.stats.input.path || '') : '';
}
function updateAbUi() {
  const a = state.slots.A, b = state.slots.B;
  const ind = $('abIndicator');
  if (state.pinned && a) {
    ind.classList.remove('hidden');
    ind.textContent = `표시: ${state.showing}  (A: ${a.runId}${b ? `, B: ${b.runId}` : ', B: 대기'})`;
    ind.className = `badge ${state.showing === 'A' ? 'warn' : 'ok'}`;
  } else ind.classList.add('hidden');
  $('btnPin').textContent = state.pinned ? 'A 고정 해제' : 'A/B 고정';
  $('btnPin').classList.toggle('active', state.pinned);
  $('btnPin').disabled = !currentRun();
  $('btnToggleAB').disabled = !(state.pinned && a && b);
}
function togglePin() {
  if (state.pinned) {
    const shown = currentRun();
    state.pinned = false; state.slots.A = null; state.slots.B = shown; state.showing = 'B';
  } else {
    const cur = currentRun();
    if (!cur) return;
    state.pinned = true; state.slots.A = cur; state.slots.B = null; state.showing = 'A';
  }
  showCurrent();
}
function toggleAB() {
  if (!(state.pinned && state.slots.A && state.slots.B)) return;
  state.showing = state.showing === 'A' ? 'B' : 'A';
  showCurrent();
}

// ------------------------------------------------------------------ panels / viewport
function layerCheckbox(key) { return $(`layer_${key}`); }
function buildLayerBar() {
  const bar = $('layerBar');
  bar.innerHTML = '';
  for (const L of LAYERS) {
    const lab = document.createElement('label');
    lab.innerHTML = `<input type="checkbox" id="layer_${L.key}" ${L.def ? 'checked' : ''}> ${esc(L.label)}`;
    lab.querySelector('input').addEventListener('change', () => { rebuildPanels(); saveForm(); });
    bar.appendChild(lab);
  }
}
function loadImage(url, onload, onerror) {
  const img = new Image();
  img.onload = onload; img.onerror = onerror;
  img.src = url;
  return img;
}
function ensureImages(run, key, wantFull) {
  const e = run.images[key] || (run.images[key] = { preview: null, full: null, previewState: 0, fullState: 0 });
  const purl = run.files[`preview_${key}`], furl = run.files[key];
  if (e.previewState === 0) {
    const url = purl || furl;
    if (url) {
      e.previewState = 1;
      e.preview = loadImage(url, () => { e.previewState = 2; if (url === furl) { e.full = e.preview; e.fullState = 2; } requestRedraw(); }, () => { e.previewState = 3; requestRedraw(); });
    } else e.previewState = 3;
  }
  if (wantFull && e.fullState === 0 && furl) {
    e.fullState = 1;
    e.full = loadImage(furl, () => { e.fullState = 2; requestRedraw(); }, () => { e.fullState = 3; requestRedraw(); });
  }
  return e;
}
function rebuildPanels() {
  const run = currentRun();
  const grid = $('panelGrid');
  grid.innerHTML = '';
  state.panels = [];
  for (const L of LAYERS) {
    const cb = layerCheckbox(L.key);
    const has = !!run && !!(run.files[L.key] || run.files[`preview_${L.key}`]);
    cb.disabled = !has;
    cb.parentElement.classList.toggle('off', !has);
    cb.parentElement.title = has ? '' : '이 실행에는 해당 산출물이 없습니다 (덤프 min 또는 참조 없음)';
  }
  if (!run) {
    grid.innerHTML = '<div class="empty">실행 결과가 없습니다. 좌측에서 입력 파일을 지정하고 [실행] 을 누르십시오.</div>';
    return;
  }
  for (const L of LAYERS) {
    const cb = layerCheckbox(L.key);
    if (cb.disabled || !cb.checked) continue;
    const el = document.createElement('div');
    el.className = 'panel';
    el.innerHTML = `<div class="panel-title"><span>${esc(L.label)}</span><span class="panel-info"></span></div><canvas></canvas>`;
    grid.appendChild(el);
    const canvas = el.querySelector('canvas');
    const p = { layer: L, el, canvas, ctx: canvas.getContext('2d'), info: el.querySelector('.panel-info') };
    state.panels.push(p);
    bindPanelEvents(p);
    ensureImages(run, L.key, false);
  }
  sizeCanvases();
  if (state.view.fitted) fitView();
  requestRedraw();
}
function imageDims() { const run = currentRun(); return run ? { W: run.stats.input.width, H: run.stats.input.height } : { W: 1, H: 1 }; }
function sizeCanvases() {
  const { W, H } = imageDims();
  for (const p of state.panels) {
    const w = Math.max(50, Math.floor(p.el.clientWidth) - 2);
    const h = Math.max(140, Math.min(520, Math.round(w * H / W)));
    if (p.canvas.width !== w || p.canvas.height !== h) { p.canvas.width = w; p.canvas.height = h; }
  }
}
function fitView() {
  const p = state.panels[0];
  if (!p) return;
  const { W, H } = imageDims();
  const s = Math.min(p.canvas.width / W, p.canvas.height / H);
  state.view = { scale: s, tx: (p.canvas.width - W * s) / 2, ty: (p.canvas.height - H * s) / 2, fitted: true };
}
function requestRedraw() {
  if (state.redrawPending) return;
  state.redrawPending = true;
  requestAnimationFrame(() => { state.redrawPending = false; redrawAll(); });
}
function redrawAll() { for (const p of state.panels) drawPanel(p); }
function drawPanel(p) {
  const run = currentRun();
  const { canvas, ctx } = p;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#101115';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!run) return;
  const { W, H } = imageDims();
  const v = state.view;
  const e = run.images[p.layer.key];
  let img = null, isFull = false, note = '';
  if (e) {
    const previewReady = e.previewState === 2, fullReady = e.fullState === 2;
    const needFull = previewReady && v.scale * (W / e.preview.naturalWidth) > 1.02;
    if (needFull && e.fullState === 0) ensureImages(run, p.layer.key, true);
    if (fullReady && (needFull || !previewReady)) { img = e.full; isFull = true; }
    else if (previewReady) { img = e.preview; if (needFull) note = e.fullState === 3 ? '원본 로드 실패' : '원본 불러오는 중…'; }
    else if (e.previewState === 3) note = '이미지를 불러올 수 없습니다';
    else note = '불러오는 중…';
  }
  if (img) {
    ctx.imageSmoothingEnabled = v.scale * (W / img.naturalWidth) < 1;
    ctx.setTransform(v.scale, 0, 0, v.scale, v.tx, v.ty);
    ctx.drawImage(img, 0, 0, W, H);
    ctx.setTransform(1, 0, 0, 1, 0, 0);
  }
  // Image border, ROI and crosshair overlays.
  ctx.strokeStyle = 'rgba(255,255,255,0.25)';
  ctx.lineWidth = 1;
  ctx.strokeRect(v.tx + 0.5, v.ty + 0.5, W * v.scale, H * v.scale);
  const roi = run.stats.params_effective && run.stats.params_effective.roi;
  if (Array.isArray(roi) && roi.length === 4) {
    ctx.strokeStyle = 'rgba(255,209,102,0.9)';
    ctx.strokeRect(v.tx + roi[0] * v.scale + 0.5, v.ty + roi[1] * v.scale + 0.5, (roi[2] - roi[0]) * v.scale, (roi[3] - roi[1]) * v.scale);
  }
  const vr = run.stats.valid_rows;
  if (vr && p.layer.key === 'raw_vis') {
    ctx.strokeStyle = 'rgba(77,163,255,0.6)';
    ctx.setLineDash([4, 4]);
    for (const y of [vr.first, vr.last + 1]) { const yy = Math.round(v.ty + y * v.scale) + 0.5; ctx.beginPath(); ctx.moveTo(v.tx, yy); ctx.lineTo(v.tx + W * v.scale, yy); ctx.stroke(); }
    ctx.setLineDash([]);
  }
  if (state.hover) {
    const hx = v.tx + (state.hover.x + 0.5) * v.scale, hy = v.ty + (state.hover.y + 0.5) * v.scale;
    ctx.strokeStyle = 'rgba(77,163,255,0.9)';
    ctx.beginPath(); ctx.moveTo(hx - 12, hy); ctx.lineTo(hx - 3, hy); ctx.moveTo(hx + 3, hy); ctx.lineTo(hx + 12, hy);
    ctx.moveTo(hx, hy - 12); ctx.lineTo(hx, hy - 3); ctx.moveTo(hx, hy + 3); ctx.lineTo(hx, hy + 12); ctx.stroke();
    if (v.scale >= 4) ctx.strokeRect(v.tx + state.hover.x * v.scale + 0.5, v.ty + state.hover.y * v.scale + 0.5, v.scale, v.scale);
  }
  if (note) { ctx.font = '12px sans-serif'; ctx.fillStyle = 'rgba(0,0,0,0.6)'; ctx.fillRect(6, canvas.height - 22, ctx.measureText(note).width + 12, 18); ctx.fillStyle = '#ddd'; ctx.fillText(note, 12, canvas.height - 9); }
  p.info.textContent = img ? (isFull ? `원본 · ${(v.scale * 100).toFixed(0)} %` : `미리보기 1/${Math.round(W / img.naturalWidth)} · ${(v.scale * 100).toFixed(0)} %`) : '';
}
function canvasPos(p, ev) { const r = p.canvas.getBoundingClientRect(); return { x: (ev.clientX - r.left) * (p.canvas.width / r.width), y: (ev.clientY - r.top) * (p.canvas.height / r.height) }; }
function toImage(pt) { const v = state.view; return { x: Math.floor((pt.x - v.tx) / v.scale), y: Math.floor((pt.y - v.ty) / v.scale) }; }
function bindPanelEvents(p) {
  const c = p.canvas;
  c.addEventListener('wheel', (ev) => {
    ev.preventDefault();
    const m = canvasPos(p, ev);
    const factor = ev.deltaY < 0 ? 1.25 : 0.8;
    const v = state.view;
    const { W } = imageDims();
    const fitScale = Math.min(c.width / W, 1);
    const ns = Math.max(fitScale * 0.1, Math.min(64, v.scale * factor));
    const f = ns / v.scale;
    v.tx = m.x - (m.x - v.tx) * f; v.ty = m.y - (m.y - v.ty) * f; v.scale = ns; v.fitted = false;
    requestRedraw();
  }, { passive: false });
  c.addEventListener('mousedown', (ev) => {
    if (ev.button !== 0) return;
    state.drag = { x: ev.clientX, y: ev.clientY, moved: false };
    c.classList.add('dragging');
    ev.preventDefault();
  });
  c.addEventListener('dblclick', () => { fitView(); requestRedraw(); });
  c.addEventListener('mousemove', (ev) => {
    if (state.drag) return;
    const ip = toImage(canvasPos(p, ev));
    const { W, H } = imageDims();
    if (ip.x < 0 || ip.y < 0 || ip.x >= W || ip.y >= H) { if (state.hover) { state.hover = null; requestRedraw(); } return; }
    if (!state.hover || state.hover.x !== ip.x || state.hover.y !== ip.y) { state.hover = ip; requestRedraw(); schedulePixel(ip.x, ip.y); }
  });
  c.addEventListener('mouseleave', () => { if (!state.drag && state.hover) { state.hover = null; requestRedraw(); } });
}
window.addEventListener('mousemove', (ev) => {
  if (!state.drag) return;
  const dx = ev.clientX - state.drag.x, dy = ev.clientY - state.drag.y;
  const p = state.panels[0];
  const k = p ? p.canvas.width / p.canvas.getBoundingClientRect().width : 1;
  state.view.tx += dx * k; state.view.ty += dy * k; state.view.fitted = false;
  state.drag.x = ev.clientX; state.drag.y = ev.clientY; state.drag.moved = true;
  requestRedraw();
});
window.addEventListener('mouseup', () => { if (state.drag) { state.drag = null; state.panels.forEach((p) => p.canvas.classList.remove('dragging')); } });

// ------------------------------------------------------------------ pixel inspector
let pixelTimer = null, pixelSeq = 0;
function schedulePixel(x, y) {
  clearTimeout(pixelTimer);
  pixelTimer = setTimeout(async () => {
    const run = currentRun();
    if (!run) return;
    const seq = ++pixelSeq;
    try {
      const j = await api(`/api/pixel?runId=${encodeURIComponent(run.runId)}&x=${x}&y=${y}`);
      if (seq !== pixelSeq) return;
      renderInspector(x, y, j);
    } catch (e) { $('insPos').textContent = `(${x}, ${y}) 조회 실패`; }
  }, 60);
}
function clearInspector() {
  for (const id of ['insPos', 'insRaw', 'insBasis', 'insDiff', 'insResult', 'insRef']) { $(id).textContent = '–'; $(id).classList.remove('null'); }
}
function renderInspector(x, y, j) {
  $('insPos').textContent = `(${x}, ${y})`;
  const raw = $('insRaw');
  if (j.raw === null || j.raw === undefined) raw.textContent = '–';
  else if (j.raw <= -900) raw.textContent = `null (${j.raw})`;
  else raw.textContent = fmt(j.raw, 3);
  raw.classList.toggle('null', j.raw !== null && j.raw !== undefined && j.raw <= -900);
  $('insBasis').textContent = fmt(j.basis, 1);
  $('insDiff').textContent = fmt(j.diff, 1);
  $('insResult').textContent = j.result === null || j.result === undefined ? '–' : String(j.result);
  const ref = $('insRef');
  if (j.ref === null || j.ref === undefined) ref.textContent = '–';
  else { const d = j.result === null ? null : j.result - j.ref; ref.textContent = d === null ? String(j.ref) : `${j.ref} (Δ ${d > 0 ? '+' : ''}${d})`; ref.classList.toggle('null', d !== null && d !== 0); }
}

// ------------------------------------------------------------------ stats / charts
function renderStats(run) {
  const el = $('statsTable');
  if (!run || !run.stats) { el.innerHTML = '<div class="hint">실행 결과가 없습니다.</div>'; return; }
  const s = run.stats;
  const g = (o, k) => (o && o[k] !== undefined ? o[k] : null);
  const pe = s.params_effective || {}, ex = pe.exp || {}, nl = s.null || {}, vr = s.valid_rows || {}, z = s.z || {}, cl = s.clip || {}, ou = s.output || {}, tm = s.timing_ms || {}, rf = s.ref;
  const expOn = Object.entries(ex).filter(([k, v]) => (k === 'null_value' ? v !== -1 : k === 'fill_holes' ? v > 0 : !!v)).map(([k, v]) => (typeof v === 'boolean' ? k : `${k}=${v}`));
  const rows = [
    ['입력', [
      ['경로', g(s.input, 'path')], ['크기', `${g(s.input, 'width')} × ${g(s.input, 'height')} (${g(s.input, 'dtype')})`], ['도구 버전', s.tool_version],
    ]],
    ['적용 파라미터', [
      ['DepthPreprocType (지그 제거 규칙)', `${pe.type || '-'} ${pe.type === 'INNERCENTER' ? '(제거 없음)' : pe.type === 'BEAD' ? '(최대 성분)' : `(침식 ${pe.break_kernel ?? 3} → 최대 성분 → 팽창)`}`],
      ['PatchSize / step', `${pe.patch_w} × ${pe.patch_h} / ${pe.step_w} × ${pe.step_h}`], ['Overlap', pe.overlap], ['StagePosition', pe.stage],
      ['ROI', Array.isArray(pe.roi) ? pe.roi.join(', ') : '미사용'], ['Lower / Upper', `${pe.lower} / ${pe.upper}`],
      ['실험 옵션', expOn.length ? expOn.join(', ') : '없음'],
      ['DLL 동일 경로', pe.dll_identical === false ? '<span class="bad">아니오 (실험 옵션)</span>' : '<span class="good">예</span>', true],
    ]],
    ['null / 지그', [
      ['원본 null', `${fmtInt(nl.count)} px (${fmtPct(nl.pct)})`], ['지그(removeStage) 제거', `${fmtInt(nl.stage_removed_count)} px (${fmtPct(nl.stage_removed_pct)})`],
      ['제거 후 유효 픽셀', fmtInt(nl.valid_after_stage)], ['유효 성분 수 / 최대 성분 비율', `${fmtInt(nl.valid_components)} / ${fmtPct(nl.largest_component_pct_of_valid)}`],
      ['유효 행 범위 (띠)', `${vr.first} ~ ${vr.last} (${vr.last - vr.first + 1} 행)`],
    ]],
    ['z / 클립', [
      ['z 최소 / 최대', `${fmt(z.min, 3)} / ${fmt(z.max, 3)}`], ['scaled 1 단위 = mm', z.unit_mm_per_scaled_unit !== null && z.unit_mm_per_scaled_unit !== undefined ? z.unit_mm_per_scaled_unit.toExponential(3) : '–'],
      ['low / high (scaled 단위)', `${fmt(cl.low, 1)} / ${fmt(cl.high, 1)}`], ['low / high (mm)', `${fmt(cl.low_mm, 4)} / ${fmt(cl.high_mm, 4)}`],
      ['정규화 min / max', `${fmt(cl.norm_min, 1)} / ${fmt(cl.norm_max, 1)}`], ['diff == 0 비율', fmtPct(cl.zero_diff_pct)],
    ]],
    ['출력 (0 = null 또는 하위 클립)', [
      ['검정(0) 전체', `${fmtInt(ou.black_count)} px (${fmtPct(ou.black_pct)})`], ['유효 내 검정(하위 클립)', `${fmtInt(ou.black_in_valid)} px (${fmtPct(ou.black_in_valid_pct)} of 유효)`],
      ['유효 내 흰색(255, 상위 클립)', `${fmtInt(ou.white_in_valid)} px (${fmtPct(ou.white_in_valid_pct)} of 유효)`],
    ]],
  ];
  if (rf) {
    rows.push(['참조(_Proc) 비교', [
      ['참조 경로', rf.path], ['정확 일치', `<span class="${rf.exact_pct >= 99.9 ? 'good' : 'bad'}">${fmtPct(rf.exact_pct, 3)}</span>`, true],
      ['±1 / ±2 이내', `${fmtPct(rf.within1_pct, 3)} / ${fmtPct(rf.within2_pct, 3)}`], ['최대 차이 / 불일치 픽셀', `${rf.max_abs} / ${fmtInt(rf.mismatch_count)}`],
      ['검정 마스크 IoU', `<span class="${rf.black_mask_iou >= 0.99 ? 'good' : 'bad'}">${fmt(rf.black_mask_iou, 5)}</span>`, true],
      ['참조만 검정 / 결과만 검정', `${fmtInt(rf.ref_black_not_result)} / ${fmtInt(rf.result_black_not_ref)}`],
    ]]);
  } else rows.push(['참조(_Proc) 비교', [['참조', '지정되지 않음']]]);
  const ms = (v) => (v === null || v === undefined ? '–' : fmtInt(v));
  rows.push(['처리 시간 (ms)', [
    ['read / removeStage / scale', `${ms(tm.read)} / ${ms(tm.remove_stage)} / ${ms(tm.scale)}`], ['basis / diff / clip+normalize', `${ms(tm.basis)} / ${ms(tm.diff)} / ${ms(tm.clip_normalize)}`], ['total', ms(tm.total)],
  ]]);
  let html = '<table>';
  for (const [title, items] of rows) {
    html += `<tr><th colspan="2">${esc(title)}</th></tr>`;
    for (const [k, v, raw] of items) html += `<tr><td class="k">${esc(k)}</td><td class="v">${raw ? v : esc(v === null || v === undefined ? '–' : v)}</td></tr>`;
  }
  el.innerHTML = html + '</table>';
}
function prepCanvas(c) {
  const w = Math.max(200, Math.floor(c.clientWidth || c.parentElement.clientWidth || 400));
  if (c.width !== w) c.width = w;
  const ctx = c.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#14161a'; ctx.fillRect(0, 0, c.width, c.height);
  ctx.font = '11px sans-serif';
  return ctx;
}
function drawHist(run) {
  const c = $('histCanvas');
  const ctx = prepCanvas(c);
  const h = run && run.stats && run.stats.diff_hist;
  if (!h || !Array.isArray(h.counts) || !h.counts.length) { ctx.fillStyle = '#888'; ctx.fillText('데이터 없음', 10, 20); return; }
  const L = 46, R = 12, T = 10, B = 26, pw = c.width - L - R, ph = c.height - T - B;
  const log = $('histLog').checked;
  const f = log ? (v) => Math.log10(1 + v) : (v) => v;
  const maxC = Math.max(...h.counts), maxF = f(maxC) || 1;
  const n = h.counts.length;
  const xOf = (val) => L + (val - h.min) / (h.max - h.min) * pw;
  ctx.fillStyle = '#7f9bc4';
  for (let i = 0; i < n; i++) {
    const bh = f(h.counts[i]) / maxF * ph;
    ctx.fillRect(L + i / n * pw, T + ph - bh, Math.max(1, pw / n - 0.5), bh);
  }
  ctx.strokeStyle = '#555'; ctx.beginPath(); ctx.moveTo(L, T + ph + 0.5); ctx.lineTo(L + pw, T + ph + 0.5); ctx.stroke();
  const line = (val, color, label, dash) => {
    if (val === null || val === undefined) return;
    const x = Math.round(xOf(Math.max(h.min, Math.min(h.max, val)))) + 0.5;
    ctx.strokeStyle = color; ctx.setLineDash(dash || []); ctx.beginPath(); ctx.moveTo(x, T); ctx.lineTo(x, T + ph); ctx.stroke(); ctx.setLineDash([]);
    if (label) { ctx.fillStyle = color; ctx.textAlign = x > L + pw / 2 ? 'right' : 'left'; ctx.fillText(label, x + (x > L + pw / 2 ? -4 : 4), T + 10); }
  };
  line(0, '#8a8f99', null, [3, 3]);
  const cl = run.stats.clip || {};
  line(cl.low, '#4da3ff', `low ${fmt(cl.low, 0)}`);
  line(cl.high, '#ff9f43', `high ${fmt(cl.high, 0)}`);
  ctx.fillStyle = '#a7adb8'; ctx.textAlign = 'left'; ctx.fillText(String(h.min), L, c.height - 8);
  ctx.textAlign = 'center'; ctx.fillText('0', xOf(0), c.height - 8);
  ctx.textAlign = 'right'; ctx.fillText(String(h.max), L + pw, c.height - 8);
  ctx.save(); ctx.translate(12, T + ph / 2); ctx.rotate(-Math.PI / 2); ctx.textAlign = 'center'; ctx.fillText(log ? 'count (log10)' : 'count', 0, 0); ctx.restore();
  ctx.textAlign = 'right'; ctx.fillText(fmtInt(maxC), L - 4, T + 8); ctx.fillText('0', L - 4, T + ph);
  ctx.textAlign = 'left';
}
function drawRowBins(run) {
  const c = $('rowBinCanvas');
  const ctx = prepCanvas(c);
  const ou = run && run.stats && run.stats.output;
  const black = ou && ou.black_pct_by_valid_row_bin, white = ou && ou.white_pct_by_valid_row_bin;
  if (!Array.isArray(black) || !black.length) { ctx.fillStyle = '#888'; ctx.fillText('데이터 없음', 10, 20); return; }
  const L = 40, R = 12, T = 12, B = 30, pw = c.width - L - R, ph = c.height - T - B;
  const n = black.length;
  const maxV = Math.max(1, Math.ceil(Math.max(...black, ...(white || [0])) / 5) * 5);
  const gw = pw / n;
  for (let i = 0; i < n; i++) {
    const x = L + i * gw;
    const bh = black[i] / maxV * ph;
    ctx.fillStyle = '#c95f5f'; ctx.fillRect(x + gw * 0.12, T + ph - bh, gw * 0.36, bh);
    if (white) { const wh = white[i] / maxV * ph; ctx.fillStyle = '#e8e8e8'; ctx.fillRect(x + gw * 0.52, T + ph - wh, gw * 0.36, wh); }
  }
  ctx.strokeStyle = '#555'; ctx.beginPath(); ctx.moveTo(L, T + ph + 0.5); ctx.lineTo(L + pw, T + ph + 0.5); ctx.stroke();
  ctx.fillStyle = '#a7adb8'; ctx.textAlign = 'right'; ctx.fillText(`${maxV} %`, L - 4, T + 8); ctx.fillText('0', L - 4, T + ph);
  const vr = run.stats.valid_rows || { first: 0, last: n };
  const rows = vr.last - vr.first + 1;
  ctx.textAlign = 'center';
  for (let i = 0; i < n; i += Math.max(1, Math.round(n / 5))) ctx.fillText(String(Math.round(vr.first + i / n * rows)), L + i * gw + gw / 2, c.height - 16);
  ctx.fillText(String(vr.last), L + pw - gw / 2, c.height - 16);
  ctx.fillText('행 (유효 띠 구간, 위 → 아래)', L + pw / 2, c.height - 4);
  ctx.fillStyle = '#c95f5f'; ctx.fillRect(L + pw - 150, T - 2, 10, 10); ctx.fillStyle = '#a7adb8'; ctx.textAlign = 'left'; ctx.fillText('검정 %', L + pw - 136, T + 7);
  ctx.fillStyle = '#e8e8e8'; ctx.fillRect(L + pw - 80, T - 2, 10, 10); ctx.fillStyle = '#a7adb8'; ctx.fillText('흰색 %', L + pw - 66, T + 7);
}

// ------------------------------------------------------------------ run / batch actions
function setBusy(b) { state.busy = b; $('btnRun').disabled = b; $('btnBatchRun').disabled = b; }
async function runSingle() {
  if (state.busy) return;
  const input = $('inputPath').value.trim();
  const st = $('runStatus');
  if (!input) { setStatus(st, '입력 파일을 지정해 주십시오.', 'err'); return; }
  const ref = $('refPath').value.trim();
  const params = gatherParams();
  setBusy(true);
  const stop = elapsedTimer(st, 'CLI 실행 중…');
  try {
    const j = await postJson('/api/run', { input, ref: ref || null, params });
    const sec = stop();
    placeRun(makeRun(j));
    setStatus(st, `완료: ${j.runId} (${sec.toFixed(1)} 초, CLI ${(j.cli.durationMs / 1000).toFixed(1)} 초)`, 'ok');
    refreshRuns();
  } catch (e) {
    stop();
    setStatus(st, `실행 실패: ${e.message}`, 'err');
  } finally { setBusy(false); }
}
async function runBatch() {
  if (state.busy) return;
  const folder = $('batchFolder').value.trim();
  const st = $('batchStatus');
  if (!folder) { setStatus(st, '타이어 폴더를 지정해 주십시오.', 'err'); return; }
  const params = Object.assign(gatherParams(), { override_all: $('batchOverride').checked });
  setBusy(true);
  const stop = elapsedTimer(st, '배치 실행 중…');
  try {
    const j = await postJson('/api/batch', { folder, params, fovproc: $('batchFovproc').value.trim() || null });
    const sec = stop();
    state.batch = j;
    renderBatch(j);
    setStatus(st, `완료: ${j.batchId} — ${j.items.length} 장 (${sec.toFixed(1)} 초)`, 'ok');
    refreshRuns();
  } catch (e) {
    stop();
    setStatus(st, `배치 실패: ${e.message}`, 'err');
  } finally { setBusy(false); }
}
function renderBatch(j) {
  const el = $('batchTable');
  if (!j || !j.items || !j.items.length) { el.innerHTML = '<div class="hint">처리된 이미지가 없습니다.</div>'; return; }
  const anyExp = j.items.some((it) => it.stats && it.stats.params_effective && it.stats.params_effective.dll_identical === false);
  let html = `<div class="hint">배치 ${esc(j.batchId)} · ${esc(j.folder || '')}${anyExp ? ' · <span class="badge warn">실험 옵션 사용 — DLL 과 다를 수 있음</span>' : ''}</div>`;
  html += '<table><tr><th>Idx</th><th>이름</th><th>결과 (result)</th><th>참조 (_Proc)</th><th>정확 일치</th><th>±1</th><th>검정 IoU</th><th>null %</th><th>유효 내 검정 %</th><th>ms</th><th></th></tr>';
  j.items.forEach((it, i) => {
    const s = it.stats, rf = s && s.ref, f = it.files || {};
    const img = (k) => (f[`preview_${k}`] || f[k] ? `<img src="${f[`preview_${k}`] || f[k]}" alt="${k}">` : '<span class="hint">없음</span>');
    html += `<tr class="item" data-i="${i}" title="클릭하면 단일 실행 탭에서 엽니다">
      <td class="num">${it.imgIdx}</td><td>${esc(it.name)}${s && s.params_effective && s.params_effective.dll_identical === false ? ' <span class="badge warn">exp</span>' : ''}${it.error ? ` <span class="badge err">${esc(it.error)}</span>` : ''}</td>
      <td>${img('result')}</td><td>${img('ref')}</td>
      <td class="num">${rf ? fmtPct(rf.exact_pct, 3) : '–'}</td><td class="num">${rf ? fmtPct(rf.within1_pct, 2) : '–'}</td><td class="num">${rf ? fmt(rf.black_mask_iou, 4) : '–'}</td>
      <td class="num">${s ? fmtPct(s.null.pct, 1) : '–'}</td><td class="num">${s ? fmtPct(s.output.black_in_valid_pct, 2) : '–'}</td><td class="num">${s ? fmtInt(s.timing_ms.total) : '–'}</td>
      <td><button class="btn small" data-open="${i}">열기</button></td></tr>`;
  });
  el.innerHTML = html + '</table>';
  el.querySelectorAll('tr.item').forEach((tr) => tr.addEventListener('click', () => openBatchItem(j.items[+tr.dataset.i])));
}
function openBatchItem(it) {
  if (!it || !it.stats) return;
  placeRun(makeRun({ runId: it.runId, outDir: it.dir, stats: it.stats, files: it.files, input: it.stats.input.path, ref: it.stats.ref ? it.stats.ref.path : null }, { kind: 'batch' }));
  switchTab('single');
  setStatus($('runStatus'), `배치 항목 열기: ${it.runId}`, 'ok');
}
async function refreshRuns() {
  try {
    const j = await api('/api/runs');
    const sel = $('prevRuns');
    const cur = sel.value;
    sel.innerHTML = '';
    for (const r of j.runs) {
      const o = document.createElement('option');
      o.value = r.runId;
      o.textContent = `${r.runId} ${r.kind === 'batch' ? '[배치]' : ''} ${r.input ? basename(r.input) : ''}${r.ok ? '' : ' (실패)'}${r.dll_identical === false ? ' (exp)' : ''}`;
      sel.appendChild(o);
    }
    if (cur) sel.value = cur;
  } catch (e) { /* ignore */ }
}
async function loadPrevRun() {
  const id = $('prevRuns').value;
  if (!id) return;
  const st = $('runStatus');
  try {
    const j = await api(`/api/run?runId=${encodeURIComponent(id)}`);
    if (j.items) { state.batch = j; renderBatch(j); switchTab('batch'); setStatus($('batchStatus'), `불러옴: ${j.batchId} — ${j.items.length} 장`, 'ok'); return; }
    placeRun(makeRun(j));
    setStatus(st, `불러옴: ${j.runId}`, 'ok');
  } catch (e) { setStatus(st, `불러오기 실패: ${e.message}`, 'err'); }
}

// ------------------------------------------------------------------ upload
function uploadFile(file) {
  const prog = $('uploadProgress'), bar = prog.querySelector('.bar'), label = prog.querySelector('.label');
  prog.classList.remove('hidden'); bar.style.width = '0%'; label.textContent = `${file.name} 업로드 중…`;
  const xhr = new XMLHttpRequest();
  xhr.open('PUT', `/api/upload?name=${encodeURIComponent(file.name)}`);
  xhr.upload.onprogress = (ev) => { if (ev.lengthComputable) { const p = ev.loaded / ev.total * 100; bar.style.width = `${p}%`; label.textContent = `${file.name} ${p.toFixed(0)} % (${fmtBytes(ev.loaded)} / ${fmtBytes(ev.total)})`; } };
  xhr.onload = () => {
    try {
      const j = JSON.parse(xhr.responseText);
      if (xhr.status >= 200 && xhr.status < 300) {
        label.textContent = `업로드 완료: ${j.path}`;
        $('inputPath').value = j.path; state.refAuto = true; onInputChanged();
      } else label.textContent = `업로드 실패: ${j.error || xhr.status}`;
    } catch (e) { label.textContent = `업로드 실패: ${xhr.status}`; }
    setTimeout(() => prog.classList.add('hidden'), 4000);
  };
  xhr.onerror = () => { label.textContent = '업로드 실패 (네트워크)'; };
  xhr.send(file);
}

// ------------------------------------------------------------------ folder browser modal
const browser = { mode: 'file', onSelect: null, path: '', parent: null };
function openBrowser(opts) {
  browser.mode = opts.mode || 'file'; browser.onSelect = opts.onSelect;
  $('browserTitle').textContent = opts.title || (browser.mode === 'dir' ? '폴더 선택' : '파일 선택');
  $('btnBrowserSelectDir').classList.toggle('hidden', browser.mode !== 'dir');
  const roots = $('browserRoots');
  roots.innerHTML = '';
  for (const r of (state.config ? state.config.roots : [])) { const b = document.createElement('button'); b.className = 'btn small'; b.textContent = r; b.addEventListener('click', () => browseTo(r)); roots.appendChild(b); }
  $('browserModal').classList.remove('hidden');
  browseTo(opts.start || '');
}
function closeBrowser() { $('browserModal').classList.add('hidden'); }
async function browseTo(p) {
  const list = $('browserList');
  list.innerHTML = '<div class="msg">불러오는 중…</div>';
  try {
    const j = await api(`/api/browse?path=${encodeURIComponent(p || '')}`);
    browser.path = j.path; browser.parent = j.parent;
    $('browserPath').value = j.path;
    $('btnBrowserUp').disabled = j.parent === null;
    $('btnBrowserSelectDir').disabled = !j.path;
    list.innerHTML = '';
    if (!j.dirs.length && !j.files.length) list.innerHTML = '<div class="msg">표시할 폴더/이미지 파일이 없습니다.</div>';
    for (const d of j.dirs) {
      const row = document.createElement('div');
      row.className = 'entry dir';
      row.innerHTML = `<span class="tag">폴더</span><span class="name">${esc(d)}</span>`;
      row.addEventListener('click', () => browseTo(j.path ? joinPath(j.path, d) : d));
      list.appendChild(row);
    }
    for (const f of j.files) {
      const row = document.createElement('div');
      const selectable = browser.mode === 'file' && f.kind !== 'ini' || browser.mode === 'ini' && f.kind === 'ini';
      row.className = `entry file${/_Proc/i.test(f.name) ? ' proc' : ''}${selectable ? '' : ' disabled'}`;
      row.innerHTML = `<span class="tag">${esc(f.kind)}</span><span class="name">${esc(f.name)}</span><span class="size">${fmtBytes(f.size)}</span>`;
      if (selectable) row.addEventListener('click', () => { if (browser.onSelect) browser.onSelect(joinPath(j.path, f.name)); closeBrowser(); });
      list.appendChild(row);
    }
  } catch (e) { list.innerHTML = `<div class="msg">오류: ${esc(e.message)}</div>`; }
}

// ------------------------------------------------------------------ tabs / wiring
function switchTab(name) {
  document.querySelectorAll('.tab-btn').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === `tab-${name}`));
  if (name === 'single') { sizeCanvases(); if (state.view.fitted) fitView(); requestRedraw(); drawHist(currentRun()); drawRowBins(currentRun()); }
}
function onInputChanged() {
  const input = $('inputPath').value.trim();
  const info = mappingFor(input);
  $('inputInfo').textContent = info ? `이미지 인덱스 ${info.idx} → 결과 ${info.map.result} (${info.map.name}, CAL${info.map.cal || '?'})` : '';
  saveForm();
  if (input) guessRef(false);
}
function wire() {
  document.querySelectorAll('.tab-btn').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));
  $('inputPath').addEventListener('change', () => { state.refAuto = !$('refPath').value.trim() || state.refAuto; onInputChanged(); });
  $('refPath').addEventListener('input', () => { state.refAuto = !$('refPath').value.trim(); saveForm(); });
  $('btnBrowseInput').addEventListener('click', () => openBrowser({ mode: 'file', start: dirname($('inputPath').value.trim()), title: '입력 파일 선택', onSelect: (p) => { $('inputPath').value = p; state.refAuto = true; onInputChanged(); } }));
  $('btnBrowseRef').addEventListener('click', () => openBrowser({ mode: 'file', start: dirname($('refPath').value.trim() || $('inputPath').value.trim()), title: '참조(_Proc) 파일 선택', onSelect: (p) => { $('refPath').value = p; state.refAuto = false; saveForm(); } }));
  $('btnGuessRef').addEventListener('click', () => guessRef(true));
  $('preset').addEventListener('change', () => { const cal = $('preset').value; if (cal) applyPreset(state.config.presets.find((p) => String(p.cal) === cal)); saveForm(); });
  ['pType', 'patchW', 'patchH', 'overlap', 'stage'].forEach((id) => $(id).addEventListener('input', () => { markCustomPreset(); updateDerived(); saveForm(); }));
  ['roiX1', 'roiY1', 'roiX2', 'roiY2', 'lower', 'upper', 'breakKernel', 'expNullValue', 'expFillHoles', 'dumpMode', 'previewN', 'pxX', 'pxY', 'batchFolder', 'batchFovproc'].forEach((id) => $(id).addEventListener('input', () => { updateDerived(); saveForm(); }));
  ['expUseIniPct', 'expValidPct', 'expMaskedMedian', 'batchOverride'].forEach((id) => $(id).addEventListener('change', () => { updateDerived(); saveForm(); }));
  $('btnRun').addEventListener('click', runSingle);
  $('btnPin').addEventListener('click', togglePin);
  $('btnToggleAB').addEventListener('click', toggleAB);
  $('btnFit').addEventListener('click', () => { fitView(); requestRedraw(); });
  $('cols').addEventListener('change', () => { $('panelGrid').style.setProperty('--cols', $('cols').value); state.view.fitted = true; sizeCanvases(); fitView(); requestRedraw(); saveForm(); });
  $('histLog').addEventListener('change', () => { drawHist(currentRun()); saveForm(); });
  $('btnBatchRun').addEventListener('click', runBatch);
  $('btnBrowseBatch').addEventListener('click', () => openBrowser({ mode: 'dir', start: $('batchFolder').value.trim() || (state.config && state.config.roots[0]) || '', title: '타이어 폴더 선택', onSelect: (p) => { $('batchFolder').value = p; saveForm(); } }));
  $('btnBrowseFovproc').addEventListener('click', () => openBrowser({ mode: 'ini', start: dirname($('batchFovproc').value.trim()), title: 'FOVPROC.ini 선택', onSelect: (p) => { $('batchFovproc').value = p; saveForm(); } }));
  $('btnLoadRun').addEventListener('click', loadPrevRun);
  $('btnRefreshRuns').addEventListener('click', refreshRuns);
  $('btnBrowserClose').addEventListener('click', closeBrowser);
  $('browserModal').addEventListener('click', (ev) => { if (ev.target === $('browserModal')) closeBrowser(); });
  $('btnBrowserGo').addEventListener('click', () => browseTo($('browserPath').value.trim()));
  $('browserPath').addEventListener('keydown', (ev) => { if (ev.key === 'Enter') browseTo($('browserPath').value.trim()); });
  $('btnBrowserUp').addEventListener('click', () => { if (browser.parent !== null) browseTo(browser.parent); });
  $('btnBrowserSelectDir').addEventListener('click', () => { if (browser.onSelect && browser.path) browser.onSelect(browser.path); closeBrowser(); });
  const dz = $('dropZone');
  ['dragenter', 'dragover'].forEach((t) => dz.addEventListener(t, (ev) => { ev.preventDefault(); dz.classList.add('over'); }));
  ['dragleave', 'drop'].forEach((t) => dz.addEventListener(t, (ev) => { ev.preventDefault(); dz.classList.remove('over'); }));
  dz.addEventListener('drop', (ev) => { const f = ev.dataTransfer.files && ev.dataTransfer.files[0]; if (f) uploadFile(f); });
  window.addEventListener('keydown', (ev) => {
    const tag = (ev.target.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
    if (ev.key === 'Escape') closeBrowser();
    if (ev.code === 'Space') { ev.preventDefault(); toggleAB(); }
    if (ev.key === '0') { fitView(); requestRedraw(); }
  });
  new ResizeObserver(() => { sizeCanvases(); if (state.view.fitted) fitView(); requestRedraw(); drawHist(currentRun()); drawRowBins(currentRun()); }).observe($('panelGrid'));
}

// Debug hook for the browser console (state inspection / forced redraw).
window.__dps = { state, redrawAll, fitView, currentRun, gatherParams };

async function init() {
  buildLayerBar();
  wire();
  try { await loadConfig(); } catch (e) { $('cliStatus').textContent = `설정 로드 실패: ${e.message}`; $('cliStatus').className = 'badge err'; }
  const saved = loadForm();
  if (saved) { writeForm(saved); $('panelGrid').style.setProperty('--cols', $('cols').value || '1'); }
  else if (state.config && state.config.presets.length) { const p = state.config.presets.find((x) => x.cal === 3) || state.config.presets[0]; $('preset').value = String(p.cal); applyPreset(p); }
  state.refAuto = !$('refPath').value.trim();
  updateDerived();
  if ($('inputPath').value.trim()) onInputChanged();
  showCurrent();
  refreshRuns();
}
init();
})();
