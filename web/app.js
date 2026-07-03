// Main-thread UI (DOM only); the wasm core lives in worker.js.
// This pass: composited layered icons, nameplate rows with remove, goal
// editing, unlink, candidate auto-pull on flows, per-bundle persistence.

const worker = new Worker('./worker.js', { type: 'module' });
let nextId = 1;
const pending = new Map();
worker.onmessage = ({ data }) => {
  const { id, result, error } = data;
  const entry = pending.get(id);
  if (!entry) return;
  pending.delete(id);
  error ? entry.reject(new Error(error)) : entry.resolve(result);
};
function rpc(method, ...args) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    const transfer = args.filter((a) => a instanceof ArrayBuffer);
    worker.postMessage({ id, method, args }, transfer);
  });
}

const $ = (sel) => document.querySelector(sel);
const status = (text) => { $('#status').textContent = text; };
const esc = (s) => s.replace(/[&<>"]/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---- icons: full layer compositing on canvas ----
const iconUrlCache = new Map();
const iconBitmapCache = new Map();
async function layerBitmap(file) {
  if (!iconBitmapCache.has(file)) {
    iconBitmapCache.set(file, (async () => {
      const bytes = await rpc('iconFile', file);
      if (!bytes) return null;
      return createImageBitmap(new Blob([bytes], { type: 'image/png' }));
    })());
  }
  return iconBitmapCache.get(file);
}
async function iconUrl(tdn) {
  if (iconUrlCache.has(tdn)) return iconUrlCache.get(tdn);
  const promise = (async () => {
    const layers = await rpc('iconLayers', tdn);
    if (!Array.isArray(layers) || layers.length === 0) return null;
    const kSize = 64;
    const canvas = new OffscreenCanvas(kSize, kSize);
    const ctx = canvas.getContext('2d');
    const baseSize = layers[0].sz * (layers[0].s || 1);
    for (const layer of layers) {
      const bitmap = await layerBitmap(layer.file);
      if (!bitmap) continue;
      // Factorio icon math: scale relative to the base layer, shift in
      // base-layer pixels; tint multiplies rgba.
      const scale = (layer.s || 1) * (layer.sz ? layers[0].sz / layer.sz : 1);
      const draw = kSize * (layer.sz * (layer.s || 1)) / baseSize;
      const off = [(layer.x || 0) * kSize / baseSize, (layer.y || 0) * kSize / baseSize];
      let source = bitmap;
      const tinted = layer.r !== 1 || layer.g !== 1 || layer.b !== 1;
      if (tinted) {
        const tint = new OffscreenCanvas(bitmap.width, bitmap.height);
        const tctx = tint.getContext('2d');
        tctx.drawImage(bitmap, 0, 0);
        tctx.globalCompositeOperation = 'multiply';
        tctx.fillStyle = `rgb(${layer.r * 255},${layer.g * 255},${layer.b * 255})`;
        tctx.fillRect(0, 0, tint.width, tint.height);
        tctx.globalCompositeOperation = 'destination-in';
        tctx.drawImage(bitmap, 0, 0);
        source = tint;
      }
      ctx.globalAlpha = layer.a ?? 1;
      ctx.drawImage(source, (kSize - draw) / 2 + off[0], (kSize - draw) / 2 + off[1],
                    draw, draw);
      ctx.globalAlpha = 1;
    }
    const blob = await canvas.convertToBlob();
    return URL.createObjectURL(blob);
  })();
  iconUrlCache.set(tdn, promise);
  return promise;
}
async function iconImg(tdn) {
  try {
    const url = await iconUrl(tdn);
    return url ? `<img src="${url}" alt="">` : '<span style="width:24px"></span>';
  } catch {
    return '<span style="width:24px"></span>';
  }
}

// ---- table state (persisted per bundle) ----
let goals = [];   // {tdn, name, perMin}
let linked = [];  // tdn[]
let rows = [];    // {tdn}
let bundleKey = null;

function persist() {
  if (bundleKey) {
    localStorage.setItem(bundleKey, JSON.stringify({ goals, linked, rows }));
  }
}
function restore() {
  const saved = bundleKey && localStorage.getItem(bundleKey);
  if (!saved) return false;
  try {
    ({ goals = [], linked = [], rows = [] } = JSON.parse(saved));
    return goals.length + linked.length + rows.length > 0;
  } catch {
    return false;
  }
}

async function rebuildAndSolve() {
  persist();
  await rpc('tableClear');
  for (const goal of goals) await rpc('tableAddLink', goal.tdn, goal.perMin);
  for (const tdn of linked) await rpc('tableAddLink', tdn, 0);
  for (const row of rows) await rpc('tableAddRecipe', row.tdn);
  renderSolve(await rpc('tableSolve'));
}

function fmt(x) {
  const abs = Math.abs(x);
  if (abs >= 10000) return (x / 1000).toFixed(1) + 'k';
  if (abs >= 1000) return (x / 1000).toFixed(2) + 'k';
  return abs >= 100 ? x.toFixed(1) : x.toFixed(2);
}
const short = (tdn) => tdn.split('.').slice(1).join('.');

async function renderSolve(result) {
  const statusNames = ['solved', 'no solution — unexplained deadlocks',
                       'numerical errors', 'unexpected error'];
  status(`${statusNames[result.status] ?? '?'} · ${rows.length} rows · ` +
         `${goals.length + linked.length} links`);

  // Goals: amber pills, click amount to edit, x to remove.
  $('#goals').innerHTML = goals.map((g, i) =>
      `<span class="pill goal">${esc(g.name)}
         <button class="small ghost amt" data-goal="${i}" title="Edit demand">${g.perMin}/min</button>
         <button class="x" data-goal-x="${i}" aria-label="Remove goal">✕</button></span>`)
      .join('') ||
      '<span class="muted">No demand set — search a goods, then “Set demand”.</span>';
  for (const btn of $('#goals').querySelectorAll('[data-goal]')) {
    btn.onclick = () => {
      const goal = goals[+btn.dataset.goal];
      const perMin = parseFloat(prompt(`Demand for ${goal.name} (per minute):`, goal.perMin));
      if (Number.isFinite(perMin) && perMin > 0) { goal.perMin = perMin; rebuildAndSolve(); }
    };
  }
  for (const btn of $('#goals').querySelectorAll('[data-goal-x]')) {
    btn.onclick = () => { goals.splice(+btn.dataset.goalX, 1); rebuildAndSolve(); };
  }

  // Recipe nameplates.
  const plates = await Promise.all(result.rows.map(async (row, i) => {
    const flows = row.flows.map((f) =>
        `<span class="flow ${f.perMin >= 0 ? 'pos' : 'neg'}">` +
        `${fmt(f.perMin)} ${esc(short(f.tdn))}</span>`).join('');
    return `<div class="plate${row.warnings ? ' warn' : ''}">
      <div class="head">${await iconImg(row.recipe.tdn)}
        <span class="name" title="${esc(row.recipe.tdn)}">${esc(row.recipe.locName)}</span>
        ${row.warnings ? '<span title="solver warning">⚠</span>' : ''}
        <span class="crafts">${fmt(row.craftsPerMin)}<small>crafts/min</small></span>
        <button class="x" data-row-x="${i}" aria-label="Remove row">✕</button>
      </div>
      <div class="flows">${flows}</div>
    </div>`;
  }));
  $('#rows').innerHTML = plates.join('');
  for (const btn of $('#rows').querySelectorAll('[data-row-x]')) {
    btn.onclick = () => { rows.splice(+btn.dataset.rowX, 1); rebuildAndSolve(); };
  }

  // Links: neutral pills; unmatched marked red; x unlinks (goals excluded).
  const linkPills = result.links.map((l) => {
    const isGoal = goals.some((g) => g.tdn === l.tdn);
    const bad = (l.flags & 1) !== 0;
    const unlink = isGoal ? '' :
        `<button class="x" data-unlink="${l.tdn}" aria-label="Unlink">✕</button>`;
    return `<span class="pill${bad ? ' bad' : ''}" title="${bad ? 'unmatched — needs both a producer and a consumer in-table' : 'matched'}">
        ${esc(l.name)} <span class="amt">${fmt(l.flow)}/min</span>${unlink}</span>`;
  });
  $('#links').innerHTML = linkPills.join('') || '<span class="muted">—</span>';
  for (const btn of $('#links').querySelectorAll('[data-unlink]')) {
    btn.onclick = () => {
      linked = linked.filter((tdn) => tdn !== btn.dataset.unlink);
      rebuildAndSolve();
    };
  }

  // Off-table flows with candidate auto-pull.
  const flowRows = await Promise.all(result.flows
      .filter((f) => Math.abs(f.perMin) > 1e-9)
      .sort((a, b) => a.perMin - b.perMin)
      .map(async (f) =>
      `<div class="row">${await iconImg(f.tdn)}` +
      `<span class="amt ${f.perMin > 0 ? 'pos' : 'neg'}">${fmt(f.perMin)}/min</span>` +
      `<span>${esc(f.name)}</span>` +
      `<button class="small" data-pull="${f.tdn}" data-side="${f.perMin < 0 ? 'p' : 'c'}">` +
      `${f.perMin < 0 ? 'produce ▸' : 'consume ▸'}</button>` +
      `<button class="small ghost" data-link="${f.tdn}">link only</button></div>`));
  $('#flows').innerHTML = flowRows.join('') || '<span class="muted">—</span>';
  for (const btn of $('#flows').querySelectorAll('[data-link]')) {
    btn.onclick = () => { linked.push(btn.dataset.link); rebuildAndSolve(); };
  }
  for (const btn of $('#flows').querySelectorAll('[data-pull]')) {
    btn.onclick = () => pullCandidates(btn.dataset.pull, btn.dataset.side === 'p');
  }
}

// ---- candidate auto-pull ----
const inTable = (tdn) => goals.some((g) => g.tdn === tdn) || linked.includes(tdn);

async function pullCandidates(tdn, wantProducers) {
  if (!inTable(tdn)) linked.push(tdn);
  const info = await rpc('goodsInfo', tdn);
  const list = wantProducers ? info.producers : info.consumers;
  if (list.length === 1) {
    rows.push({ tdn: list[0].tdn });
    await rebuildAndSolve();
    return;
  }
  // Rank: candidates touching goods already in-table first, then simpler ones.
  const score = (r) => {
    const side = wantProducers ? r.in : r.out;
    return side.filter((x) => inTable(x.tdn)).length * 100 - side.length;
  };
  const ranked = [...list].sort((a, b) => score(b) - score(a));
  await rebuildAndSolve();
  renderRecipeList(
      `${wantProducers ? 'Produce' : 'Consume'} ${info.goods.locName} — pick a recipe`,
      ranked);
}

async function renderRecipeList(title, list, header = '') {
  const items = await Promise.all(list.map(async (r) =>
      `<div class="row">${await iconImg(r.tdn)}` +
      `<span title="${esc(r.tdn)}">${esc(r.locName)}</span>` +
      `<span class="muted mono">${r.time}s</span>` +
      `<button class="small" data-add="${r.tdn}">+ row</button></div>` +
      `<div class="muted" style="margin-left:32px;font-size:12.5px">` +
      `${r.in.map((x) => `${x.amount} ${esc(x.name)}`).join(', ') || 'no inputs'} → ` +
      `${r.out.map((x) => `${x.amount} ${esc(x.name)}`).join(', ')}</div>`));
  $('#recipeInfo').innerHTML =
      `${header}<div class="eyebrow">${esc(title)}</div>` + items.join('');
  for (const btn of $('#recipeInfo').querySelectorAll('[data-add]')) {
    btn.onclick = () => { rows.push({ tdn: btn.dataset.add }); rebuildAndSolve(); };
  }
}

// ---- search / goods info ----
$('#search').oninput = async (e) => {
  const query = e.target.value.trim();
  if (query.length < 2) { $('#results').innerHTML = ''; return; }
  const results = await rpc('searchGoods', query, 20);
  const html = await Promise.all(results.map(async (g) =>
      `<div class="row goods" data-tdn="${g.tdn}" style="cursor:pointer" tabindex="0">` +
      `${await iconImg(g.tdn)}<span>${esc(g.locName)}</span>` +
      `<span class="muted">${esc(g.kind)}</span></div>`));
  $('#results').innerHTML = html.join('');
  for (const el of $('#results').querySelectorAll('[data-tdn]')) {
    const open = () => showGoods(el.dataset.tdn);
    el.onclick = open;
    el.onkeydown = (ev) => { if (ev.key === 'Enter') open(); };
  }
};

async function showGoods(tdn) {
  const info = await rpc('goodsInfo', tdn);
  await renderRecipeList(`Producers (${info.producers.length})`, info.producers,
      `<div class="eyebrow">${esc(info.goods.locName)}</div>
       <button id="goalBtn">Set demand…</button>`);
  $('#goalBtn').onclick = () => {
    const perMin = parseFloat(prompt(`Demand for ${info.goods.locName} (per minute):`, '900'));
    if (Number.isFinite(perMin) && perMin > 0) {
      goals.push({ tdn, name: info.goods.locName, perMin });
      rebuildAndSolve();
    }
  };
}

// ---- bundle loading ----
$('#loadBtn').onclick = () => $('#bundleFile').click();
$('#bundleFile').onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  status(`loading ${file.name} (${(file.size / 1e6).toFixed(1)} MB)…`);
  const info = await rpc('loadBundle', await file.arrayBuffer());
  if (info.error) { status(`load failed: ${info.error}`); return; }
  status(`${info.objects} objects · ${info.recipes} recipes · factorio ${info.meta.factorioVersion}`);
  bundleKey = 'yafc:' + JSON.stringify(info.meta.mods ?? file.name);
  $('#search').disabled = false;
  $('#dropHint').hidden = true;
  $('#workspace').hidden = false;
  if (restore()) rebuildAndSolve(); else renderSolve({ status: 0, rows: [], links: [], flows: [] });
};
$('#clearBtn').onclick = () => {
  goals = []; linked = []; rows = [];
  rebuildAndSolve();
};
