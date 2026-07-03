// Main-thread UI (DOM only); the wasm core lives in worker.js.
// This pass: composited layered icons, nameplate rows with remove, goal
// editing, unlink, candidate auto-pull on flows, per-bundle persistence.

// One-time storage migration: the app shipped briefly as "yafc web" before
// the mancos rename; carry projects/settings across without data loss.
for (const key of Object.keys(localStorage)) {
  if (key.startsWith('yafc:')) {
    const renamed = 'mancos:' + key.slice('yafc:'.length);
    if (localStorage.getItem(renamed) === null) {
      localStorage.setItem(renamed, localStorage.getItem(key));
    }
  }
}

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
// The beta channel is served under /beta/ from the beta branch.
if (location.pathname.includes('/beta')) {
  document.querySelector('h1')?.insertAdjacentHTML('beforeend',
      ' <span style="font-size:10px;color:var(--signal-amber);letter-spacing:.2em">BETA</span>');
}
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
      // Icon files may append mip levels beside the base image (120x64 =
      // 64+32+16+8): draw only the base square.
      const srcSide = Math.min(bitmap.width, bitmap.height);
      let source = bitmap;
      const tinted = layer.r !== 1 || layer.g !== 1 || layer.b !== 1;
      if (tinted) {
        const tint = new OffscreenCanvas(srcSide, srcSide);
        const tctx = tint.getContext('2d');
        tctx.drawImage(bitmap, 0, 0, srcSide, srcSide, 0, 0, srcSide, srcSide);
        tctx.globalCompositeOperation = 'multiply';
        tctx.fillStyle = `rgb(${layer.r * 255},${layer.g * 255},${layer.b * 255})`;
        tctx.fillRect(0, 0, tint.width, tint.height);
        tctx.globalCompositeOperation = 'destination-in';
        tctx.drawImage(bitmap, 0, 0, srcSide, srcSide, 0, 0, srcSide, srcSide);
        source = tint;
      }
      ctx.globalAlpha = layer.a ?? 1;
      ctx.drawImage(source, 0, 0, srcSide, srcSide,
                    (kSize - draw) / 2 + off[0], (kSize - draw) / 2 + off[1],
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

// ---- table state: pages of {name, goals, linked, rows}; the active page's
// fields are aliased into goals/linked/rows for the render/solve paths ----
let pages = [newPage('Page 1')];
let activePage = 0;
let goals = pages[0].goals;   // {tdn, name, perMin}
let linked = pages[0].linked; // tdn[]
let rows = pages[0].rows;     // {tdn}
let bundleKey = null;

function newPage(name) {
  return { name, goals: [], linked: [], rows: [], linkAlgos: {} };
}
// Sparse {tdn: 1|2}: 1 = over-production allowed, 2 = over-consumption
// allowed; absent = exact match (desktop per-link setting).
const linkAlgoOf = (tdn) => pages[activePage].linkAlgos?.[tdn] ?? 0;
function setLinkAlgo(tdn, algo) {
  const page = pages[activePage];
  page.linkAlgos ??= {};
  if (algo) page.linkAlgos[tdn] = algo;
  else delete page.linkAlgos[tdn];
}
function bindActivePage() {
  const page = pages[activePage];
  goals = page.goals;
  linked = page.linked;
  rows = page.rows;
}
function setActivePage(index) {
  activePage = Math.max(0, Math.min(index, pages.length - 1));
  bindActivePage();
  renderPageTabs();
  rebuildAndSolve();
}
let research = { filter: false, techs: [] };

async function pushResearch() {
  research = await rpc('setResearch', JSON.stringify(research));
  if (bundleKey) localStorage.setItem(bundleKey + ':research', JSON.stringify(research));
}

// ---- undo/redo: pages-state snapshots captured on every persisted change.
// Tab switches update the tracked state silently (not undoable themselves),
// but each undo restores the page that was active when that edit was made.
const undoStack = [];
const redoStack = [];
let lastPagesJson = null;   // change detector (pages only)
let lastSnapshot = null;    // full state incl. activePage
let restoringHistory = false;

function updateUndoButtons() {
  $('#undoBtn').disabled = undoStack.length === 0;
  $('#redoBtn').disabled = redoStack.length === 0;
}

function recordHistory() {
  const pagesJson = JSON.stringify(pages);
  const state = JSON.stringify({ pages, activePage });
  if (lastPagesJson !== null && pagesJson !== lastPagesJson) {
    undoStack.push(lastSnapshot);
    if (undoStack.length > 100) undoStack.shift();
    redoStack.length = 0;
  }
  lastPagesJson = pagesJson;
  lastSnapshot = state;
  updateUndoButtons();
}

function applySnapshot(text) {
  const parsed = JSON.parse(text);
  pages = parsed.pages;
  activePage = Math.min(parsed.activePage ?? 0, pages.length - 1);
  lastPagesJson = JSON.stringify(pages);
  lastSnapshot = text;
  bindActivePage();
  renderPageTabs();
}

async function timeTravel(fromStack, toStack) {
  if (fromStack.length === 0) return;
  toStack.push(JSON.stringify({ pages, activePage }));
  restoringHistory = true;
  applySnapshot(fromStack.pop());
  await rebuildAndSolve();
  restoringHistory = false;
  updateUndoButtons();
}
const undo = () => timeTravel(undoStack, redoStack);
const redo = () => timeTravel(redoStack, undoStack);
$('#undoBtn').onclick = undo;
$('#redoBtn').onclick = redo;
document.addEventListener('keydown', (e) => {
  if (!(e.ctrlKey || e.metaKey)) return;
  const tag = document.activeElement?.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
  const key = e.key.toLowerCase();
  if (key === 'z') {
    e.preventDefault();
    e.shiftKey ? redo() : undo();
  } else if (key === 'y') {
    e.preventDefault();
    redo();
  }
});

function persist() {
  if (!restoringHistory) recordHistory();
  if (bundleKey) {
    localStorage.setItem(bundleKey, JSON.stringify({ pages, activePage }));
  }
}
function restore() {
  const saved = bundleKey && localStorage.getItem(bundleKey);
  if (!saved) return false;
  try {
    const parsed = JSON.parse(saved);
    if (Array.isArray(parsed.pages) && parsed.pages.length > 0) {
      pages = parsed.pages;
      activePage = Math.min(parsed.activePage ?? 0, pages.length - 1);
    } else {
      // Migrate the old single-table shape.
      pages = [{ name: 'Page 1', goals: parsed.goals ?? [],
                 linked: parsed.linked ?? [], rows: parsed.rows ?? [] }];
      activePage = 0;
    }
    bindActivePage();
    renderPageTabs();
    return pages.some((p) => p.goals.length + p.linked.length + p.rows.length > 0);
  } catch {
    return false;
  }
}

// ---- page tabs ----
function renderPageTabs() {
  const tabs = pages.map((p, i) =>
      `<button class="small${i === activePage ? '' : ' ghost'}" data-page="${i}"
         title="Double-click to rename">${esc(p.name)}</button>`).join('');
  $('#pageTabs').innerHTML = tabs +
      ` <button class="small ghost" id="addPage" title="Add page">+</button>` +
      (pages.length > 1 ? ` <button class="x" id="removePage" title="Remove page">✕</button>` : '');
  for (const btn of $('#pageTabs').querySelectorAll('[data-page]')) {
    btn.onclick = () => setActivePage(+btn.dataset.page);
    btn.ondblclick = () => {
      const name = prompt('Page name:', pages[+btn.dataset.page].name);
      if (name) { pages[+btn.dataset.page].name = name; renderPageTabs(); persist(); }
    };
  }
  $('#addPage').onclick = () => {
    pages.push(newPage(`Page ${pages.length + 1}`));
    setActivePage(pages.length - 1);
  };
  const remove = $('#removePage');
  if (remove) {
    remove.onclick = () => {
      if (!confirm(`Remove page "${pages[activePage].name}"?`)) return;
      pages.splice(activePage, 1);
      setActivePage(Math.max(0, activePage - 1));
    };
  }
}

async function rebuildAndSolve() {
  persist();
  await rpc('tableClear');
  const filler = pages[activePage].filler;
  if (filler) await rpc('tableSetFiller', JSON.stringify(filler));
  // Table units are per SECOND (desktop yafc compatible); UI shows /min.
  for (const goal of goals) {
    await rpc('tableAddLink', goal.tdn, goal.perMin / 60, linkAlgoOf(goal.tdn));
  }
  for (const tdn of linked) await rpc('tableAddLink', tdn, 0, linkAlgoOf(tdn));
  // Rows carry per-project choices ('' / missing = favorite-or-first
  // defaults): a reactor burning mox vs uranium cells is a different chain.
  for (const row of rows) {
    await rpc('tableAddRecipe', row.tdn, JSON.stringify({
      fixed: (row.fixed ?? 0) / 60,
      entity: row.entity ?? '',
      fuel: row.fuel ?? '',
      modules: row.modules,
    }));
  }
  renderSolve(await rpc('tableSolve'));
}

const cost = (c) => c > 0 ? `<span class="muted mono">\u00a4${c >= 100 ? c.toFixed(0) : c.toFixed(1)}</span>` : '';

const WARN_ALARM = (1 << 16) | (1 << 17) | (1 << 18);  // solver warnings -> orange plate
function warningText(bits) {
  const parts = [];
  if (bits & (1 << 16)) parts.push('Deadlock: this loop needs an external source to start');
  if (bits & (1 << 17)) parts.push('Overproduction required: a linked byproduct cannot be fully consumed');
  if (bits & (1 << 18)) parts.push('Needs more buildings than built');
  if (bits & (1 << 2)) parts.push('Fuel consumption limited: craft time stretched to match fuel input cap');
  if (bits & (1 << 6)) parts.push('Productivity capped at this recipe\'s maximum');
  if (bits & (1 << 8)) parts.push('No crafter selected — using raw recipe time');
  if (bits & (1 << 9)) parts.push('No fuel selected — energy shown as raw power draw');
  if (bits & (1 << 10)) parts.push('Fuel temperature exceeds the maximum this building can use');
  if (bits & (1 << 11)) parts.push('Selected fuel provides no energy to this building');
  if (bits & (1 << 12)) parts.push('Temperature-based fuel is not linked, energy unknown');
  if (bits & (1 << 0)) parts.push('Assumes Nauvis day/night solar ratio');
  return parts.join(' · ') || 'solver warning';
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
      `<span class="pill goal${g.perMin < 0 ? ' input' : ''}">` +
      `<button class="chip" data-goods="${g.tdn}">${esc(g.name)}</button>
         <button class="small ghost amt" data-goal="${i}" title="Edit demand">${g.perMin}/min</button>
         <button class="x" data-goal-x="${i}" aria-label="Remove goal">✕</button></span>`)
      .join('') ||
      '<span class="muted">No demand set — search a goods, then “Set demand”.</span>';
  for (const btn of $('#goals').querySelectorAll('[data-goal]')) {
    btn.onclick = () => {
      const goal = goals[+btn.dataset.goal];
      const perMin = parseFloat(prompt(
          `Demand for ${goal.name} in units/min\n(negative = input goal: consume this much)`,
          goal.perMin));
      if (Number.isFinite(perMin) && perMin !== 0) { goal.perMin = perMin; rebuildAndSolve(); }
    };
  }
  for (const btn of $('#goals').querySelectorAll('[data-goal-x]')) {
    btn.onclick = () => { goals.splice(+btn.dataset.goalX, 1); rebuildAndSolve(); };
  }

  // Recipe nameplates.
  const plates = await Promise.all(result.rows.map(async (row, i) => {
    const flows = (await Promise.all(row.flows.map(async (f) =>
        `<button class="flow chip ${f.perMin >= 0 ? 'pos' : 'neg'}" data-goods="${f.tdn}"` +
        ` title="${esc(f.tdn)}">${await iconImg(f.tdn)}` +
        `${fmt(f.perMin * 60)}</button>`))).join('');
    const entity = row.entity && row.entity.tdn ? `<button class="entity chip" data-config="${i}"` +
        ` title="${esc(row.entity.locName)} — click to change building, fuel, modules">` +
        `${await iconImg(row.entity.tdn)}<span class="amt">×${fmt(row.buildings)}</span>` +
        `${row.entity.powerMw > 0 ? `<span class="muted amt">${fmt(row.entity.powerMw * row.buildings * 1000)}kW</span>` : ''}` +
        `</button>` :
        `<button class="cfg" data-config="${i}" title="Configure building, fuel, modules">⚙</button>`;
    const mods = (await Promise.all((row.modules ?? []).map(async (m) =>
        `<span title="${esc(m.locName)} ×${m.count}">${await iconImg(m.tdn)}` +
        `${m.count > 1 ? `<small>${m.count}</small>` : ''}</span>`))).join('');
    const beacon = row.beacon && row.beacon.tdn
        ? `<span title="${esc(row.beacon.locName)} ×${row.beacon.count}">` +
          `${await iconImg(row.beacon.tdn)}<small>${row.beacon.count}</small></span>` : '';
    return `<div class="plate${row.warnings & WARN_ALARM ? ' warn' : ''}">
      <div class="head">${await iconImg(row.recipe.tdn)}
        <span class="name" title="${esc(row.recipe.tdn)}">${esc(row.recipe.locName)}</span>
        ${entity}
        ${mods || beacon ? `<span class="mods">${mods}${beacon}</span>` : ''}
        ${row.warnings ? `<span title="${esc(warningText(row.warnings))}">⚠</span>` : ''}
        <button class="crafts${rows[i]?.fixed ? ' pinned' : ''}" data-pin="${i}"
          title="${rows[i]?.fixed ? 'Rate pinned — click to change/unpin' : 'Click to pin this rate'}">
          ${rows[i]?.fixed ? '📌 ' : ''}${fmt(row.craftsPerMin * 60)}<small>crafts/min</small></button>
        <button class="x" data-row-x="${i}" aria-label="Remove row">✕</button>
      </div>
      <div class="flows">${flows}</div>
    </div>`;
  }));
  $('#rows').innerHTML = plates.join('');
  for (const btn of $('#rows').querySelectorAll('[data-row-x]')) {
    btn.onclick = () => { rows.splice(+btn.dataset.rowX, 1); rebuildAndSolve(); };
  }
  for (const btn of $('#rows').querySelectorAll('[data-config]')) {
    btn.onclick = () => openRowConfig(+btn.dataset.config);
  }
  for (const btn of $('#rows').querySelectorAll('[data-pin]')) {
    btn.onclick = () => {
      const row = rows[+btn.dataset.pin];
      const current = row.fixed ? String(row.fixed) : '';
      const answer = prompt('Pin rate in crafts/min (empty to unpin):', current);
      if (answer === null) return;
      const value = parseFloat(answer);
      if (answer.trim() === '' || !Number.isFinite(value) || value <= 0) delete row.fixed;
      else row.fixed = value;
      rebuildAndSolve();
    };
  }

  // Links: neutral pills; unmatched marked red; x unlinks (goals excluded).
  // The =/≥/≤ toggle cycles the link algorithm (match / over-production OK /
  // over-consumption OK) — key for recursive chains with byproducts.
  const ALGO = [
    { symbol: '=', label: 'Exact match — click to allow over-production' },
    { symbol: '≥', label: 'Over-production allowed — click to allow over-consumption' },
    { symbol: '≤', label: 'Over-consumption allowed — click for exact match' },
  ];
  const linkPills = result.links.map((l) => {
    const isGoal = goals.some((g) => g.tdn === l.tdn);
    const bad = (l.flags & 1) !== 0;
    const algo = l.algo ?? 0;
    const unlink = isGoal ? '' :
        `<button class="x" data-unlink="${l.tdn}" aria-label="Unlink">✕</button>`;
    return `<span class="pill${bad ? ' bad' : ''}${algo ? ' loose' : ''}" title="${bad ? 'unmatched — needs both a producer and a consumer in-table' : 'matched'}">
        <button class="chip" data-goods="${l.tdn}">${esc(l.name)}</button>
        <button class="small ghost algo" data-algo="${l.tdn}" title="${ALGO[algo].label}">${ALGO[algo].symbol}</button>
        <span class="amt">${fmt(l.flow * 60)}/min</span>${unlink}</span>`;
  });
  $('#links').innerHTML = linkPills.join('') || '<span class="muted">—</span>';
  for (const btn of $('#links').querySelectorAll('[data-unlink]')) {
    btn.onclick = () => {
      pages[activePage].linked = linked.filter((tdn) => tdn !== btn.dataset.unlink);
      setLinkAlgo(btn.dataset.unlink, 0);
      bindActivePage();
      rebuildAndSolve();
    };
  }
  for (const btn of $('#links').querySelectorAll('[data-algo]')) {
    btn.onclick = () => {
      setLinkAlgo(btn.dataset.algo, (linkAlgoOf(btn.dataset.algo) + 1) % 3);
      rebuildAndSolve();
    };
  }

  // Off-table flows with candidate auto-pull.
  const flowRows = await Promise.all(result.flows
      .filter((f) => Math.abs(f.perMin) > 1e-9)
      .sort((a, b) => a.perMin - b.perMin)
      .map(async (f) =>
      `<div class="row">${await iconImg(f.tdn)}` +
      `<span class="amt ${f.perMin > 0 ? 'pos' : 'neg'}">${fmt(f.perMin * 60)}/min</span>` +
      `<button class="chip" data-goods="${f.tdn}">${esc(f.name)}</button>` +
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

// Desktop yafc's core gesture: click any goods, anywhere, to explore it.
document.addEventListener('click', (e) => {
  const el = e.target.closest('[data-goods]');
  if (!el) return;
  setTab(true);
  if (window.innerWidth <= 760) {
    $('#left').classList.add('open');
    $('#catalogFab').textContent = '✕';
  }
  showGoods(el.dataset.goods);
});

async function pullCandidates(tdn, wantProducers) {
  if (!inTable(tdn)) linked.push(tdn);
  const info = await rpc('goodsInfo', tdn);
  const list = wantProducers ? info.producers : info.consumers;
  // API pre-sorts: available first, then yafc cost ascending. Auto-pick never
  // grabs a recipe that is research- or milestone-locked, or a barreling/
  // voiding pseudo-recipe.
  const available = list.filter((r) =>
      r.available !== false && !r.milestone && !r.inaccessible && !r.special);
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

// Milestone/accessibility badge (desktop paints the milestone icon on locked
// objects). Applies to anything carrying the brief's milestone fields.
async function lockBadge(o) {
  if (o.inaccessible) {
    return '<span class="muted" title="Not reachable with this modpack’s data">\u{1F6AB}</span>';
  }
  if (o.milestone) {
    return `<span class="ms-lock" title="Beyond milestone: ${esc(o.milestone.locName)}">` +
           `${await iconImg(o.milestone.tdn)}</span>`;
  }
  return '';
}

async function renderRecipeList(title, list, header = '') {
  const items = await Promise.all(list.map(async (r) =>
      `<div class="row"${r.inaccessible ? ' style="opacity:.55"' : ''}>${await iconImg(r.tdn)}` +
      `<span title="${esc(r.tdn)}">${esc(r.locName)}</span>` +
      `${await lockBadge(r)}` +
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
document.addEventListener('keydown', (e) => {
  if (e.key === '/' && document.activeElement?.tagName !== 'INPUT') {
    e.preventDefault();
    setTab(true);
    $('#search').focus();
  }
});
$('#search').onkeydown = (e) => {
  if (e.key === 'Enter') $('#results [data-tdn]')?.click();
  if (e.key === 'Escape') { e.target.value = ''; $('#results').innerHTML = ''; }
};
$('#search').oninput = async (e) => {
  const query = e.target.value.trim();
  if (query.length < 2) { $('#results').innerHTML = ''; return; }
  const results = await rpc('searchGoods', query, 20);
  const html = await Promise.all(results.map(async (g) =>
      `<div class="row goods" data-tdn="${g.tdn}" style="cursor:pointer" tabindex="0">` +
      `${await iconImg(g.tdn)}<span>${esc(g.locName)}</span>${await lockBadge(g)}` +
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
    const perMin = parseFloat(prompt(
        `Demand for ${info.goods.locName} in units/min\n(negative = input goal: consume this much)`,
        '900'));
    if (Number.isFinite(perMin) && perMin !== 0) {
      goals.push({ tdn, name: info.goods.locName, perMin });
      rebuildAndSolve();
    }
  };
}

// ---- projects: import/export/share (human priorities 1+2) ----
// zlib format on both paths: the browser's Compression Streams API when
// available, otherwise miniz inside the wasm module — wire-compatible, so a
// link minted on either path opens on the other.
async function deflateBase64Url(text) {
  let bytes;
  if (typeof CompressionStream !== 'undefined') {
    const stream = new Blob([text]).stream()
        .pipeThrough(new CompressionStream('deflate'));
    bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  } else {
    const buffer = await rpc('zlib', 'deflate', new TextEncoder().encode(text).buffer);
    bytes = new Uint8Array(buffer);
  }
  let binary = '';
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}
async function inflateBase64Url(encoded) {
  const binary = atob(encoded.replaceAll('-', '+').replaceAll('_', '/'));
  const bytes = Uint8Array.from(binary, (c) => c.charCodeAt(0));
  if (typeof DecompressionStream !== 'undefined') {
    const stream = new Blob([bytes]).stream()
        .pipeThrough(new DecompressionStream('deflate'));
    return new Response(stream).text();
  }
  const buffer = await rpc('zlib', 'inflate', bytes.buffer);
  return new TextDecoder().decode(buffer);
}

async function applyLoadedProject(state) {
  if (state.error) { status(`project load failed: ${state.error}`); return; }
  pages = state.pages.map((p, i) => ({
    name: p.name || `Page ${i + 1}`,
    goals: p.goals.map((g) => ({ tdn: g.tdn, name: g.name, perMin: g.perMin })),
    linked: p.linked,
    rows: p.rows,          // rows keep entity/fuel/modules mirrors verbatim
    filler: p.filler,      // page-level module defaults, if any
    linkAlgos: p.linkAlgos ?? {},
  }));
  activePage = 0;
  bindActivePage();
  renderPageTabs();
  if (state.errors?.length) status(`project loaded with ${state.errors.length} warnings`);
  await rebuildAndSolve();
}

async function importProjectText(text) {
  applyLoadedProject(await rpc('projectLoad', text));
}

$('#shareBtn').onclick = async () => {
  persist();
  const text = await rpc('projectSaveRaw', JSON.stringify(pages));
  const encoded = await deflateBase64Url(text);
  // Fragment, not query: never sent to the server, so no URL-length limits
  // (or log leakage) apply; the link doubles as a bookmark of the table.
  const url = `${location.origin}${location.pathname}#p=${encoded}`;
  try {
    await navigator.clipboard.writeText(url);
    status(`share link copied (${url.length} chars)`);
  } catch {
    prompt('Share link:', url);
  }
};
$('#exportBtn').onclick = async () => {
  persist();
  const text = await rpc('projectSaveRaw', JSON.stringify(pages));
  const a2 = document.createElement('a');
  a2.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a2.download = 'factory.yafc';
  a2.click();
};
$('#importBtn').onclick = () => $('#projectFile').click();
$('#projectFile').onchange = (e) => {
  const input = e.target;
  const file = input.files[0];
  if (!file) return;
  // FileReader with the input reset only AFTER the read settles: clearing
  // input.value while a read is in flight invalidates the File in Firefox
  // (NotFoundError). Reads can still fail on exotic mounts (sandboxed
  // shared folders) — offer paste as the escape hatch.
  const reader = new FileReader();
  reader.onerror = () => {
    input.value = '';
    status(`could not read ${file.name}: ${reader.error?.message} — ` +
           `try copying it to a local folder, or paste it`);
    const text = prompt('Paste the project file contents instead:');
    if (text) importProjectText(text);
  };
  reader.onload = () => {
    input.value = '';
    importProjectText(reader.result);
  };
  reader.readAsText(file);
};

function sharedPayloadFromUrl() {
  if (location.hash.startsWith('#p=')) return location.hash.slice(3);
  // Legacy links used ?p= before the move to fragments.
  return new URLSearchParams(location.search).get('p');
}

async function loadProjectFromUrl() {
  const encoded = sharedPayloadFromUrl();
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

// Fragment-only navigation does not reload the page: pasting a different
// share link into an open tab must still load that project.
window.addEventListener('hashchange', () => {
  if (bundleKey && location.hash.startsWith('#p=')) loadProjectFromUrl();
});

// ---- language: browser auto-detection + selector ----
function pickLanguage(available) {
  const saved = localStorage.getItem('mancos:lang');
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
  localStorage.setItem('mancos:lang', lang);
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

// ---- row config: building / fuel / modules / beacons + ☆ defaults ----
// Favorites are the "default building/fuel" mechanism (desktop: starred
// objects float to the top and win default picks). Per-bundle persisted.
let favorites = new Set();

async function pushFavorites() {
  await rpc('setDefaults', JSON.stringify({ favorites: [...favorites] }));
  if (bundleKey) {
    localStorage.setItem(bundleKey + ':favorites', JSON.stringify([...favorites]));
  }
}

let rowConfig = null;  // working copy while the dialog is open

async function openRowConfig(i) {
  const row = rows[i];
  if (!row) return;
  const modules = row.modules ?? {};
  rowConfig = {
    rowIndex: i,
    recipeTdn: row.tdn,
    entity: row.entity ?? '',
    fuel: row.fuel ?? '',
    list: (modules.list ?? []).map((m) => ({ ...m })),
    beacon: modules.beacon ?? '',
    beaconList: (modules.beaconList ?? []).map((m) => ({ ...m })),
  };
  await renderRowConfig();
  $('#rowDialog').showModal();
}

async function renderRowConfig() {
  const opts = await rpc('rowOptions', rowConfig.recipeTdn, rowConfig.entity || '');
  if (opts.error) { status(opts.error); return; }
  rowConfig.entity = opts.entity ?? '';
  const effectiveFuel = rowConfig.fuel ||
      (opts.fuels.find((f) => f.favorite) ?? opts.fuels[0])?.tdn || '';

  const star = (o) =>
      `<button class="fav${o.favorite ? ' on' : ''}" data-fav="${o.tdn}"
         title="${o.favorite ? 'Unset default' : 'Set as default'}"
         aria-label="Toggle default">${o.favorite ? '★' : '☆'}</button>`;
  const optRow = async (o, kind, sel, meta) =>
      `<div style="display:flex;align-items:center">${star(o)}
       <button class="opt${sel ? ' sel' : ''}" data-${kind}="${o.tdn}">
         ${await iconImg(o.tdn)}<span>${esc(o.locName)}</span>
         <span class="meta">${meta}</span></button></div>`;

  const crafters = (await Promise.all(opts.crafters.map((c) =>
      optRow(c, 'pick-entity', c.tdn === rowConfig.entity,
             `×${c.speed}${c.moduleSlots ? ` · ${c.moduleSlots} slots` : ''}` +
             `${c.powerMw > 0 ? ` · ${fmt(c.powerMw * 1000)}kW` : ''}`)))).join('');
  const fuels = (await Promise.all(opts.fuels.map((f) =>
      optRow(f, 'pick-fuel', f.tdn === effectiveFuel, `${f.fuelValue}MJ`)))).join('');

  // Live effects preview mirroring the C++ ApplyModules math: entries in
  // order, count 0 takes all remaining slots; beacons apply efficiency x
  // profile(ceil(total/slots)).
  const selectedBeacon = opts.beacons.find((b) => b.tdn === rowConfig.beacon);
  const beaconTotal = rowConfig.beaconList.reduce((sum, e) => sum + e.count, 0);
  const beaconCount = selectedBeacon && beaconTotal > 0
      ? Math.ceil(beaconTotal / (selectedBeacon.moduleSlots || 1)) : 0;
  const preview = (() => {
    const specs = new Map(opts.modules.map((m) => [m.tdn, m]));
    const eff = { speed: 0, productivity: 0, consumption: 0 };
    let remaining = opts.moduleSlots;
    for (const e of rowConfig.list) {
      const spec = specs.get(e.tdn);
      if (!spec || remaining <= 0) continue;
      const count = e.count === 0 ? remaining : Math.min(e.count, remaining);
      remaining -= count;
      eff.speed += (spec.speed || 0) * count;
      eff.productivity += (spec.productivity || 0) * count;
      eff.consumption += (spec.consumption || 0) * count;
    }
    if (selectedBeacon && beaconCount > 0) {
      const profile = selectedBeacon.profile?.length
          ? selectedBeacon.profile[Math.min(beaconCount - 1, selectedBeacon.profile.length - 1)]
          : 1;
      const strength = selectedBeacon.efficiency * profile;
      for (const e of rowConfig.beaconList) {
        const spec = selectedBeacon.modules.find((m) => m.tdn === e.tdn);
        if (!spec) continue;
        eff.speed += (spec.speed || 0) * strength * e.count;
        eff.consumption += (spec.consumption || 0) * strength * e.count;
        if ((spec.productivity || 0) > 0) {
          eff.productivity += spec.productivity * strength * e.count;
        }
      }
    }
    return { ...eff, remaining: Math.max(0, remaining) };
  })();
  const pct = (x) => `${x > 0 ? '+' : ''}${Math.round(x * 100)}%`;
  const previewLine = `speed ${pct(preview.speed)} · productivity ${pct(preview.productivity)}` +
      ` · energy ${pct(preview.consumption)}` +
      (rowConfig.list.some((e) => e.count === 0)
          ? '' : ` · ${preview.remaining} slot${preview.remaining === 1 ? '' : 's'} free`);

  const specMeta = (m) =>
      [m.speed ? `spd${pct(m.speed)}` : '',
       m.productivity ? `prod${pct(m.productivity)}` : '',
       m.consumption ? `enrg${pct(m.consumption)}` : '']
      .filter(Boolean).join(' ');

  // Template entries: count editable in place (0 = fill remaining slots).
  const entryRows = async (entries, kind) => (await Promise.all(entries.map(async (e, i) =>
      `<div class="row modentry">${await iconImg(e.tdn)}
         <span>${esc(e.locName ?? short(e.tdn))}</span>
         <input type="number" min="0" step="1" value="${e.count}" data-${kind}-count="${i}"
                aria-label="Module count">
         <span class="muted">${e.count === 0 && kind === 'mod' ? 'fills remaining' : ''}</span>
         <button class="x" data-${kind}-x="${i}" aria-label="Remove">✕</button>
       </div>`))).join('');

  const modulePalette = (await Promise.all(opts.modules.map((m) =>
      optRow(m, 'add-module', false, specMeta(m))))).join('');
  const beacons = (await Promise.all(opts.beacons.map((b) =>
      optRow(b, 'pick-beacon', b.tdn === rowConfig.beacon,
             `${b.moduleSlots} slots · eff ${(+b.efficiency.toFixed(2))}`)))).join('');
  const beaconModulePalette = selectedBeacon
      ? (await Promise.all(selectedBeacon.modules.map((m) =>
          optRow(m, 'add-beacon-module', false, specMeta(m))))).join('')
      : '';

  $('#rowDialogBody').innerHTML = `
    <div class="eyebrow">Building</div><div class="opts">${crafters}</div>
    ${opts.hasEnergy && opts.fuels.length ? `<div class="eyebrow">Fuel</div><div class="opts">${fuels}</div>` : ''}
    ${opts.moduleSlots > 0 || rowConfig.list.length ? `
      <div class="eyebrow">Modules — ${opts.moduleSlots} slots</div>
      ${rowConfig.list.length
          ? await entryRows(rowConfig.list, 'mod')
          : '<div class="muted" style="padding:2px 6px">No modules — pick from the list to add.</div>'}
      <div class="opts">${modulePalette}</div>` : ''}
    ${opts.beacons.length ? `
      <div class="eyebrow">Beacons</div>
      <div class="opts">
        <div style="display:flex;align-items:center"><span class="fav"></span>
          <button class="opt${!rowConfig.beacon ? ' sel' : ''}" data-pick-beacon="">
          <span style="width:22px"></span><span>No beacons</span></button></div>
        ${beacons}</div>
      ${selectedBeacon ? `
        ${rowConfig.beaconList.length
            ? await entryRows(rowConfig.beaconList, 'bmod') +
              `<div class="muted" style="padding:0 6px">counts are totals across all beacons` +
              ` — ${beaconTotal} modules = ${beaconCount} beacon${beaconCount === 1 ? '' : 's'}</div>`
            : '<div class="muted" style="padding:2px 6px">Pick a beacon module:</div>'}
        <div class="opts">${beaconModulePalette}</div>` : ''}` : ''}
    ${opts.moduleSlots > 0 || selectedBeacon ? `
      <div class="eyebrow">Net effects</div>
      <div class="mono" style="font-size:12.5px;padding:0 6px">${previewLine}</div>` : ''}`;

  const body = $('#rowDialogBody');
  for (const btn of body.querySelectorAll('[data-fav]')) {
    btn.onclick = async () => {
      favorites.has(btn.dataset.fav) ? favorites.delete(btn.dataset.fav)
                                     : favorites.add(btn.dataset.fav);
      await pushFavorites();
      renderRowConfig();
    };
  }
  for (const btn of body.querySelectorAll('[data-pick-entity]')) {
    btn.onclick = () => {
      rowConfig.entity = btn.dataset.pickEntity;
      rowConfig.fuel = '';  // re-derive for the new building
      renderRowConfig();
    };
  }
  for (const btn of body.querySelectorAll('[data-pick-fuel]')) {
    btn.onclick = () => { rowConfig.fuel = btn.dataset.pickFuel; renderRowConfig(); };
  }
  // Palette click appends an entry (first entry defaults to fill-remaining);
  // clicking a module already in the template bumps its fixed count instead.
  const addEntry = (entries, tdn, locName, defaultCount) => {
    const existing = entries.find((e) => e.tdn === tdn);
    if (existing) {
      if (existing.count > 0) existing.count += 1;
    } else {
      entries.push({ tdn, locName, count: defaultCount });
    }
    renderRowConfig();
  };
  for (const btn of body.querySelectorAll('[data-add-module]')) {
    btn.onclick = () => {
      const m = opts.modules.find((x) => x.tdn === btn.dataset.addModule);
      addEntry(rowConfig.list, m.tdn, m.locName, rowConfig.list.length === 0 ? 0 : 1);
    };
  }
  for (const btn of body.querySelectorAll('[data-add-beacon-module]')) {
    btn.onclick = () => {
      const m = selectedBeacon.modules.find((x) => x.tdn === btn.dataset.addBeaconModule);
      addEntry(rowConfig.beaconList, m.tdn, m.locName,
               selectedBeacon.moduleSlots || 1);
    };
  }
  const wireEntries = (kind, entries) => {
    for (const input of body.querySelectorAll(`[data-${kind}-count]`)) {
      input.onchange = () => {
        const i = +input.dataset[kind === 'mod' ? 'modCount' : 'bmodCount'];
        entries[i].count = Math.max(0, Math.round(+input.value || 0));
        renderRowConfig();
      };
    }
    for (const btn of body.querySelectorAll(`[data-${kind}-x]`)) {
      btn.onclick = () => {
        const i = +btn.dataset[kind === 'mod' ? 'modX' : 'bmodX'];
        entries.splice(i, 1);
        renderRowConfig();
      };
    }
  };
  wireEntries('mod', rowConfig.list);
  wireEntries('bmod', rowConfig.beaconList);
  for (const btn of body.querySelectorAll('[data-pick-beacon]')) {
    btn.onclick = () => {
      rowConfig.beacon = btn.dataset.pickBeacon;
      if (!rowConfig.beacon) rowConfig.beaconList = [];
      renderRowConfig();
    };
  }
}

$('#rowDialogApply').onclick = () => {
  const row = rows[rowConfig.rowIndex];
  if (row) {
    row.entity = rowConfig.entity || undefined;
    row.fuel = rowConfig.fuel || undefined;
    const template = {
      list: rowConfig.list,
      beacon: rowConfig.beacon || undefined,
      beaconList: rowConfig.beacon ? rowConfig.beaconList.filter((m) => m.count > 0) : [],
    };
    if (template.list.length || template.beacon) row.modules = template;
    else delete row.modules;
    rebuildAndSolve();
  }
  $('#rowDialog').close();
};
$('#rowDialogCancel').onclick = () => $('#rowDialog').close();

// ---- milestones (desktop "Milestones" dialog) ----
let milestones = [];  // [{tdn, locName, unlocked}] in discovery order

async function pushMilestones() {
  const unlocked = milestones.filter((m) => m.unlocked).map((m) => m.tdn);
  milestones = await rpc('setMilestones', JSON.stringify({ unlocked }));
  if (bundleKey) {
    localStorage.setItem(bundleKey + ':milestones', JSON.stringify(unlocked));
  }
  renderMilestonesSummary();
}

function renderMilestonesSummary() {
  const reached = milestones.filter((m) => m.unlocked).length;
  $('#milestonesSummary').textContent =
      milestones.length ? `${reached}/${milestones.length} reached` : '';
}

async function renderMilestoneGrid() {
  const grid = $('#milestoneGrid');
  grid.innerHTML = (await Promise.all(milestones.map(async (m, i) =>
      `<button class="ms${m.unlocked ? ' on' : ''}" data-ms="${i}"
         title="${esc(m.locName)}" aria-pressed="${m.unlocked}">
         ${await iconImg(m.tdn)}</button>`))).join('');
  for (const btn of grid.querySelectorAll('[data-ms]')) {
    btn.onclick = async () => {
      const m = milestones[+btn.dataset.ms];
      m.unlocked = !m.unlocked;
      await pushMilestones();
      renderMilestoneGrid();
    };
  }
}

async function setAllMilestones(unlocked) {
  for (const m of milestones) m.unlocked = unlocked;
  await pushMilestones();
  renderMilestoneGrid();
}

async function openMilestones() {
  await renderMilestoneGrid();
  renderMilestonesSummary();
  $('#milestonesDialog').showModal();
}
$('#milestonesBtn').onclick = openMilestones;
$('#milestonesAll').onclick = () => setAllMilestones(true);
$('#milestonesNone').onclick = () => setAllMilestones(false);
$('#milestonesDone').onclick = () => $('#milestonesDialog').close();
$('#milestonesDialog').onclose = () => {
  // Persist even when untouched so the dialog only auto-opens once per pack.
  if (bundleKey) {
    localStorage.setItem(bundleKey + ':milestones', JSON.stringify(
        milestones.filter((m) => m.unlocked).map((m) => m.tdn)));
  }
  // Lock badges in the open search list may be stale now.
  const search = $('#search');
  if (search.value.trim().length >= 2) search.oninput({ target: search });
};

// ---- bundle loading ----
$('#loadBtn').onclick = () => $('#bundleFile').click();
$('#bundleFile').onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  await loadBundleBuffer(await file.arrayBuffer(), file.name, null);
};

// Server-hosted packs (human priority 3): manifest + persisted default.
// Sources in order: same-origin bundles/ (local dev, self-hosting), then the
// mancos-data Pages site (the published app ships no bundles of its own;
// github.io serves with open CORS).
const PACK_SOURCES = ['./', 'https://hackcasual.github.io/mancos-data/'];
let packBase = './';

async function initPacks() {
  let manifest = null;
  for (const base of PACK_SOURCES) {
    try {
      const response = await fetch(base + 'bundles/manifest.json');
      if (!response.ok) continue;
      const parsed = await response.json();
      if (parsed?.packs?.length) {
        manifest = parsed;
        packBase = base;
        break;
      }
    } catch { /* try the next source */ }
  }
  if (!manifest?.packs?.length) return;

  const last = localStorage.getItem('mancos:lastPack');
  const packList = manifest.packs.map((p) =>
      `<button data-pack="${p.id}" data-file="${esc(p.file)}">` +
      `${esc(p.name)} <span class="muted mono">${(p.bytes / 1e6).toFixed(0)} MB</span>` +
      `</button>`).join(' ');
  $('#dropHint').insertAdjacentHTML('afterbegin',
      `<p><strong>Choose a modpack</strong></p><p>${packList}</p>
       <p class="muted">or load your own bundle below, or
       <a href="./bundler.html">build one from your game files</a></p>`);
  for (const btn of $('#dropHint').querySelectorAll('[data-pack]')) {
    btn.onclick = () => loadPack(btn.dataset.pack, btn.dataset.file);
  }
  const defaultPack = manifest.packs.find((p) => p.id === last);
  if (defaultPack) loadPack(defaultPack.id, defaultPack.file);
}

async function loadPack(id, file) {
  status(`fetching ${id}…`);
  const response = await fetch(packBase + file);
  if (!response.ok) { status(`fetch failed: ${response.status}`); return; }
  await loadBundleBuffer(await response.arrayBuffer(), id, id);
}

async function loadBundleBuffer(buffer, label, packId) {
  status(`loading ${label} (${(buffer.byteLength / 1e6).toFixed(1)} MB)…`);
  const info = await rpc('loadBundle', buffer);
  if (info.error) { status(`load failed: ${info.error}`); return; }
  if (packId) localStorage.setItem('mancos:lastPack', packId);
  status(`${info.objects} objects · ${info.recipes} recipes · factorio ${info.meta.factorioVersion}`);
  bundleKey = 'mancos:' + JSON.stringify(info.meta.mods ?? label);
  // History is per-bundle: undoing into another pack's pages would resolve
  // against the wrong database.
  undoStack.length = 0;
  redoStack.length = 0;
  lastPagesJson = null;
  lastSnapshot = null;
  updateUndoButtons();
  try {
    research = JSON.parse(localStorage.getItem(bundleKey + ':research')) ??
               { filter: false, techs: [] };
  } catch { research = { filter: false, techs: [] }; }
  await pushResearch();
  $('#researchFilter').checked = research.filter;
  try {
    favorites = new Set(JSON.parse(localStorage.getItem(bundleKey + ':favorites')) ?? []);
  } catch { favorites = new Set(); }
  if (favorites.size) await pushFavorites();
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
    if (restore()) {
      rebuildAndSolve();
    } else {
      // Fresh pack with no saved project: start clean — never carry the
      // previous pack's pages into this bundle's state.
      pages = [newPage('Page 1')];
      activePage = 0;
      bindActivePage();
      renderPageTabs();
      renderSolve({ status: 0, rows: [], links: [], flows: [] });
    }
  }
  recordHistory();  // baseline so the very first edit is undoable
  for (const id of ['shareBtn', 'exportBtn', 'importBtn', 'switchBtn']) {
    document.getElementById(id).disabled = false;
  }
  // Milestones last: the first call runs the accessibility walks in the
  // worker, so the initial solve above is never queued behind them. First
  // load of a pack opens the dialog, like desktop's new-project flow.
  let savedMilestones = null;
  try {
    savedMilestones = JSON.parse(localStorage.getItem(bundleKey + ':milestones'));
  } catch { /* treat as first visit */ }
  milestones = await rpc('setMilestones',
                         JSON.stringify({ unlocked: savedMilestones ?? [] }));
  renderMilestonesSummary();
  if (!Array.isArray(savedMilestones) && milestones.length > 0) openMilestones();
}

// ---- switch pack: close the current project, back to the chooser ----
// The project is already persisted per bundle on every change; picking this
// pack again later restores it exactly. Clearing lastPack means a reload
// also lands on the chooser instead of auto-loading.
$('#switchBtn').onclick = () => {
  persist();
  localStorage.removeItem('mancos:lastPack');
  bundleKey = null;
  pages = [newPage('Page 1')];
  activePage = 0;
  bindActivePage();
  undoStack.length = 0;
  redoStack.length = 0;
  lastPagesJson = null;
  lastSnapshot = null;
  milestones = [];
  favorites = new Set();
  for (const id of ['shareBtn', 'exportBtn', 'importBtn', 'switchBtn']) {
    document.getElementById(id).disabled = true;
  }
  $('#search').disabled = true;
  $('#search').value = '';
  $('#results').innerHTML = '';
  $('#recipeInfo').innerHTML = '';
  $('#techResults').innerHTML = '';
  $('#milestonesSummary').textContent = '';
  updateUndoButtons();
  $('#workspace').hidden = true;
  $('#dropHint').hidden = false;
  status('no bundle loaded — pick a pack');
};

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
  pages[activePage] = newPage(pages[activePage].name);
  bindActivePage();
  rebuildAndSolve();
};
