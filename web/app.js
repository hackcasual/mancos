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
let research = { filter: false, techs: [] };

async function pushResearch() {
  research = await rpc('setResearch', JSON.stringify(research));
  if (bundleKey) localStorage.setItem(bundleKey + ':research', JSON.stringify(research));
}

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

const cost = (c) => c > 0 ? `<span class="muted mono">\u00a4${c >= 100 ? c.toFixed(0) : c.toFixed(1)}</span>` : '';

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
  // API pre-sorts: available first, then yafc cost ascending.
  const available = list.filter((r) => r.available !== false);
  if (available.length === 1) {
    rows.push({ tdn: available[0].tdn });
    await rebuildAndSolve();
    return;
  }
  await rebuildAndSolve();
  renderRecipeList(
      `${wantProducers ? 'Produce' : 'Consume'} ${info.goods.locName} — pick a recipe`,
      list);
}

async function renderRecipeList(title, list, header = '') {
  const items = await Promise.all(list.map(async (r) =>
      `<div class="row">${await iconImg(r.tdn)}` +
      `<span title="${esc(r.tdn)}">${esc(r.locName)}</span>` +
      `${r.available === false ? `<span title="requires: ${esc((r.unlockedBy || []).join(', '))}">\u{1F512}</span>` : ''}` +
      `${cost(r.cost)}<span class="muted mono">${r.time}s</span>` +
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

// ---- projects: import/export/share (human priorities 1+2) ----
async function deflateBase64Url(text) {
  const stream = new Blob([text]).stream()
      .pipeThrough(new CompressionStream('deflate-raw'));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  let binary = '';
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}
async function inflateBase64Url(encoded) {
  const binary = atob(encoded.replaceAll('-', '+').replaceAll('_', '/'));
  const bytes = Uint8Array.from(binary, (c) => c.charCodeAt(0));
  const stream = new Blob([bytes]).stream()
      .pipeThrough(new DecompressionStream('deflate-raw'));
  return new Response(stream).text();
}

async function applyLoadedProject(state) {
  if (state.error) { status(`project load failed: ${state.error}`); return; }
  goals = state.goals.map((g) => ({ tdn: g.tdn, name: g.name, perMin: g.perMin }));
  linked = state.linked;
  rows = state.rows;
  if (state.errors?.length) status(`project loaded with ${state.errors.length} warnings`);
  await rebuildAndSolve();
}

async function importProjectText(text) {
  applyLoadedProject(await rpc('projectLoad', text));
}

$('#shareBtn').onclick = async () => {
  const text = await rpc('projectSaveRaw');
  const encoded = await deflateBase64Url(text);
  const url = `${location.origin}${location.pathname}?p=${encoded}`;
  try {
    await navigator.clipboard.writeText(url);
    status(`share link copied (${url.length} chars)`);
  } catch {
    prompt('Share link:', url);
  }
};
$('#exportBtn').onclick = async () => {
  const text = await rpc('projectSaveRaw');
  const a2 = document.createElement('a');
  a2.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a2.download = 'factory.yafc';
  a2.click();
};
$('#importBtn').onclick = () => $('#projectFile').click();
$('#projectFile').onchange = async (e) => {
  const file = e.target.files[0];
  if (file) importProjectText(await file.text());
};

async function loadProjectFromUrl() {
  const encoded = new URLSearchParams(location.search).get('p');
  if (!encoded) return false;
  try {
    await importProjectText(await inflateBase64Url(encoded));
    status('shared project loaded');
    return true;
  } catch (error) {
    status(`shared project failed: ${error.message}`);
    return false;
  }
}

// ---- language: browser auto-detection + selector ----
function pickLanguage(available) {
  const saved = localStorage.getItem('yafc:lang');
  if (saved && available.includes(saved)) return saved;
  // navigator.languages: try exact match (zh-CN), then base code (de-DE -> de).
  for (const pref of navigator.languages ?? [navigator.language]) {
    if (available.includes(pref)) return pref;
    const base = pref.split('-')[0];
    if (available.includes(base)) return base;
  }
  return available.includes('en') ? 'en' : available[0];
}

async function applyLanguage(lang) {
  await rpc('setLanguage', lang);
  localStorage.setItem('yafc:lang', lang);
  iconUrlCache.clear();  // names changed; icon canvases are name-independent but cheap
  await rebuildAndSolve();
  $('#results').innerHTML = '';
  $('#recipeInfo').innerHTML = '';
}

function renderLanguageSelect(available, current) {
  const display = new Intl.DisplayNames(navigator.languages ?? ['en'],
                                        { type: 'language' });
  const label = (code) => {
    try { return display.of(code.replace('-', '-')) ?? code; } catch { return code; }
  };
  const select = document.createElement('select');
  select.id = 'langSelect';
  select.ariaLabel = 'Language';
  for (const code of available) {
    const option = document.createElement('option');
    option.value = code;
    option.textContent = `${label(code)} (${code})`;
    option.selected = code === current;
    select.append(option);
  }
  select.onchange = () => applyLanguage(select.value);
  const existing = $('#langSelect');
  if (existing) existing.replaceWith(select);
  else $('#loadBtn').before(select);
}

// ---- research tab ----
$('#tabCatalog').onclick = () => setTab(true);
$('#tabResearch').onclick = () => setTab(false);
function setTab(catalog) {
  $('#catalogPanel').hidden = !catalog;
  $('#researchPanel').hidden = catalog;
  $('#tabCatalog').classList.toggle('ghost', !catalog);
  $('#tabResearch').classList.toggle('ghost', catalog);
}

$('#researchFilter').onchange = async (e) => {
  research.filter = e.target.checked;
  await pushResearch();
};
$('#techSearch').oninput = async (e) => {
  const query = e.target.value.trim();
  if (query.length < 2) { $('#techResults').innerHTML = ''; return; }
  renderTechs(await rpc('searchTechs', query, 60));
};
function renderTechs(list) {
  // Group leveled families: one row, level buttons 0..N (0 = unresearched);
  // researching level k implies the lower levels via prerequisite closure.
  const families = new Map();
  for (const t of list) {
    if (!families.has(t.family)) families.set(t.family, []);
    families.get(t.family).push(t);
  }
  const rows = [...families.values()].map((members) => {
    members.sort((a, b) => a.level - b.level);
    if (members.length === 1 && members[0].level === 0) {
      const t = members[0];
      return `<label class="row" style="cursor:pointer">
         <input type="checkbox" data-tech="${t.tdn}" ${t.researched ? 'checked' : ''}>
         <span>${esc(t.locName)}</span>
         <span class="muted mono">${t.unlocks}r</span></label>`;
    }
    const current = members.filter((m) => m.researched).at(-1);
    const baseName = members[0].locName.replace(/\s*(\d+|[Mm][Kk]\s*\d+)$/, '');
    const buttons = ['<button class="small' + (current ? ' ghost' : '') +
                     `" data-level-clear="${members[0].family}">0</button>`]
        .concat(members.map((m) =>
        `<button class="small${m.researched && m === current ? '' : ' ghost'}"
           data-level="${m.tdn}" data-family="${m.family}"
           title="${esc(m.locName)}">${m.level}</button>`));
    return `<div class="row"><span>${esc(baseName)}</span>
        <span class="muted mono">lv</span>${buttons.join('')}</div>`;
  });
  $('#techResults').innerHTML = rows.join('') || '<div class="muted">no matches</div>';

  const refresh = async () => {
    await pushResearch();
    renderTechs(await rpc('searchTechs', $('#techSearch').value.trim(), 60));
  };
  for (const box of $('#techResults').querySelectorAll('[data-tech]')) {
    box.onchange = () => {
      if (box.checked) research.techs.push(box.dataset.tech);
      else research.techs = research.techs.filter((t) => t !== box.dataset.tech);
      refresh();
    };
  }
  for (const btn of $('#techResults').querySelectorAll('[data-level]')) {
    btn.onclick = () => {
      // Selecting level k: drop this family's techs, add the level-k tech
      // (closure researches the lower levels).
      research.techs = research.techs.filter((t) =>
          t !== 'Technology.' + btn.dataset.family &&
          !t.startsWith('Technology.' + btn.dataset.family + '-'));
      research.techs.push(btn.dataset.level);
      refresh();
    };
  }
  for (const btn of $('#techResults').querySelectorAll('[data-level-clear]')) {
    btn.onclick = () => {
      research.techs = research.techs.filter((t) =>
          t !== 'Technology.' + btn.dataset.levelClear &&
          !t.startsWith('Technology.' + btn.dataset.levelClear + '-'));
      refresh();
    };
  }
}

// ---- bundle loading ----
$('#loadBtn').onclick = () => $('#bundleFile').click();
$('#bundleFile').onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  await loadBundleBuffer(await file.arrayBuffer(), file.name, null);
};

// Server-hosted packs (human priority 3): manifest + persisted default.
async function initPacks() {
  let manifest = null;
  try {
    const response = await fetch('bundles/manifest.json');
    if (response.ok) manifest = await response.json();
  } catch { /* no hosted packs: file-only mode */ }
  if (!manifest?.packs?.length) return;

  const last = localStorage.getItem('yafc:lastPack');
  const packList = manifest.packs.map((p) =>
      `<button data-pack="${p.id}" data-file="${esc(p.file)}">` +
      `${esc(p.name)} <span class="muted mono">${(p.bytes / 1e6).toFixed(0)} MB</span>` +
      `</button>`).join(' ');
  $('#dropHint').insertAdjacentHTML('afterbegin',
      `<p><strong>Choose a modpack</strong></p><p>${packList}</p>
       <p class="muted">or load your own bundle below</p>`);
  for (const btn of $('#dropHint').querySelectorAll('[data-pack]')) {
    btn.onclick = () => loadPack(btn.dataset.pack, btn.dataset.file);
  }
  const defaultPack = manifest.packs.find((p) => p.id === last);
  if (defaultPack) loadPack(defaultPack.id, defaultPack.file);
}

async function loadPack(id, file) {
  status(`fetching ${id}…`);
  const response = await fetch(file);
  if (!response.ok) { status(`fetch failed: ${response.status}`); return; }
  await loadBundleBuffer(await response.arrayBuffer(), id, id);
}

async function loadBundleBuffer(buffer, label, packId) {
  status(`loading ${label} (${(buffer.byteLength / 1e6).toFixed(1)} MB)…`);
  const info = await rpc('loadBundle', buffer);
  if (info.error) { status(`load failed: ${info.error}`); return; }
  if (packId) localStorage.setItem('yafc:lastPack', packId);
  status(`${info.objects} objects · ${info.recipes} recipes · factorio ${info.meta.factorioVersion}`);
  bundleKey = 'yafc:' + JSON.stringify(info.meta.mods ?? label);
  try {
    research = JSON.parse(localStorage.getItem(bundleKey + ':research')) ??
               { filter: false, techs: [] };
  } catch { research = { filter: false, techs: [] }; }
  await pushResearch();
  $('#researchFilter').checked = research.filter;
  const languages = await rpc('listLanguages');
  if (Array.isArray(languages) && languages.length > 0) {
    const lang = pickLanguage(languages);
    if (lang !== 'en') await rpc('setLanguage', lang);
    renderLanguageSelect(languages, lang);
  }
  $('#search').disabled = false;
  $('#dropHint').hidden = true;
  $('#workspace').hidden = false;
  const fromUrl = await loadProjectFromUrl();
  if (!fromUrl) {
    if (restore()) rebuildAndSolve();
    else renderSolve({ status: 0, rows: [], links: [], flows: [] });
  }
  for (const id of ['shareBtn', 'exportBtn', 'importBtn']) {
    document.getElementById(id).disabled = false;
  }
}

// Mobile: the catalog is a bottom sheet behind the floating button.
$('#catalogFab').onclick = () => {
  const open = $('#left').classList.toggle('open');
  $('#catalogFab').textContent = open ? '✕' : '+';
  if (open) $('#search').focus();
};
// Adding a row or setting a goal closes the sheet so the table is visible.
const closeSheet = () => {
  $('#left').classList.remove('open');
  $('#catalogFab').textContent = '+';
};
document.addEventListener('click', (e) => {
  if (window.innerWidth > 760) return;
  if (e.target.matches('[data-add], #goalBtn')) closeSheet();
});

initPacks();
$('#clearBtn').onclick = () => {
  goals = []; linked = []; rows = [];
  rebuildAndSolve();
};
