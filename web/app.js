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

// Small icon badge for a quality brief ({tdn, locName, level, ...} or null/
// undefined). Normal (level 0) renders nothing — the overwhelming default
// case should look identical to a table with no quality tiers in use.
async function qualityBadge(quality) {
  if (!quality || quality.level === 0) return '';
  return `<span class="quality-badge" title="${esc(quality.locName)}">${await iconImg(quality.tdn)}</span>`;
}
const qualityByTdn = (tdn) => qualities.find((q) => q.tdn === tdn);

// ---- table state: projects of {name, pages, activePage, settings}; each
// project's settings (productivity research levels etc.) apply to every
// solve in it — different projects model e.g. different research states,
// which matters enormously for quality recycling loops. The active project's
// pages are aliased into pages/activePage, and the active page's fields into
// goals/linked/rows, for the render/solve paths. ----
let projects = [newProject('Project 1')];
let activeProject = 0;
let pages = projects[0].pages;
let activePage = 0;
let goals = pages[0].goals;   // {tdn, name, perMin, quality?}
let linked = pages[0].linked; // tdn[]
let rows = pages[0].rows;     // {tdn}
let bundleKey = null;
let currentPackId = null;  // manifest pack id of the loaded bundle, or null for a local file

function newPage(name) {
  return { name, goals: [], linked: [], rows: [], linkAlgos: {} };
}
// Productivity fractions (0.1 = +10%), matching .yafc ProjectSettings units;
// the settings dialog displays them as percentages.
function defaultSettings() {
  return { miningProductivity: 0, researchProductivity: 0, productivityLevels: {},
           hideUnreachable: false };
}
function newProject(name) {
  return { name, pages: [newPage('Page 1')], activePage: 0, settings: defaultSettings() };
}
const projSettings = () => projects[activeProject].settings ??= defaultSettings();
// Per-project display preference: drop statically-unreachable objects (the
// 🚫 tier — not obtainable with this pack's data, e.g. disabled-mod content
// or the quality-unknown sentinel) from search results, recipe lists and
// pickers. Milestone-LOCKED items (🔒) always stay visible — they're normal
// progression targets. Lives only in local project state, not .yafc (a
// mancos display preference, not a desktop-compatible model field).
const hideUnreachable = () => !!projSettings().hideUnreachable;
const reachable = (list) => hideUnreachable() ? list.filter((o) => !o.inaccessible) : list;

// Linked-goods entries: plain "Item.x" for Normal, "Item.x!Quality.y" for a
// quality-tiered link (also the linkAlgos key format). '!' never appears in
// typeDotNames, so the split is unambiguous.
const linkedKey = (tdn, qualityTdn) =>
    qualityTdn ? `${tdn}!${qualityTdn}` : tdn;
function parseLinked(entry) {
  const bang = entry.indexOf('!');
  return bang < 0 ? { tdn: entry, quality: '' }
                  : { tdn: entry.slice(0, bang), quality: entry.slice(bang + 1) };
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
  const project = projects[activeProject];
  pages = project.pages;
  activePage = Math.max(0, Math.min(project.activePage ?? 0, pages.length - 1));
  const page = pages[activePage];
  goals = page.goals;
  linked = page.linked;
  rows = page.rows;
}
function setActivePage(index) {
  projects[activeProject].activePage =
      Math.max(0, Math.min(index, pages.length - 1));
  bindActivePage();
  renderPageTabs();
  rebuildAndSolve();
}
function setActiveProject(index) {
  activeProject = Math.max(0, Math.min(index, projects.length - 1));
  bindActivePage();
  renderPageTabs();
  rebuildAndSolve();
}
let research = { filter: false, techs: [] };

async function pushResearch() {
  research = await rpc('setResearch', JSON.stringify(research));
  if (bundleKey) localStorage.setItem(bundleKey + ':research', JSON.stringify(research));
}

// ---- undo/redo: projects-state snapshots captured on every persisted
// change. Tab/project switches update the tracked state silently (not
// undoable themselves), but each undo restores the project+page that were
// active when that edit was made.
const undoStack = [];
const redoStack = [];
let lastProjectsJson = null;  // change detector (projects only)
let lastSnapshot = null;      // full state incl. activeProject
let restoringHistory = false;

function updateUndoButtons() {
  $('#undoBtn').disabled = undoStack.length === 0;
  $('#redoBtn').disabled = redoStack.length === 0;
}

function recordHistory() {
  const projectsJson = JSON.stringify(projects);
  const state = JSON.stringify({ projects, activeProject });
  if (lastProjectsJson !== null && projectsJson !== lastProjectsJson) {
    undoStack.push(lastSnapshot);
    if (undoStack.length > 100) undoStack.shift();
    redoStack.length = 0;
  }
  lastProjectsJson = projectsJson;
  lastSnapshot = state;
  updateUndoButtons();
}

function applySnapshot(text) {
  const parsed = JSON.parse(text);
  projects = parsed.projects;
  activeProject = Math.min(parsed.activeProject ?? 0, projects.length - 1);
  lastProjectsJson = JSON.stringify(projects);
  lastSnapshot = text;
  bindActivePage();
  renderPageTabs();
}

async function timeTravel(fromStack, toStack) {
  if (fromStack.length === 0) return;
  toStack.push(JSON.stringify({ projects, activeProject }));
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
    localStorage.setItem(bundleKey, JSON.stringify({ projects, activeProject }));
  }
}
function restore() {
  const saved = bundleKey && localStorage.getItem(bundleKey);
  if (!saved) return false;
  try {
    const parsed = JSON.parse(saved);
    if (Array.isArray(parsed.projects) && parsed.projects.length > 0) {
      projects = parsed.projects;
      activeProject = Math.min(parsed.activeProject ?? 0, projects.length - 1);
    } else if (Array.isArray(parsed.pages) && parsed.pages.length > 0) {
      // Migrate the single-project {pages, activePage} shape.
      projects = [{ name: 'Project 1', pages: parsed.pages,
                    activePage: parsed.activePage ?? 0, settings: defaultSettings() }];
      activeProject = 0;
    } else {
      // Migrate the oldest single-table shape.
      projects = [{ name: 'Project 1',
                    pages: [{ name: 'Page 1', goals: parsed.goals ?? [],
                              linked: parsed.linked ?? [], rows: parsed.rows ?? [] }],
                    activePage: 0, settings: defaultSettings() }];
      activeProject = 0;
    }
    bindActivePage();
    renderPageTabs();
    return projects.some((proj) => proj.pages.some(
        (p) => p.goals.length + p.linked.length + p.rows.length > 0));
  } catch {
    return false;
  }
}

// ---- project selector + page tabs ----
function renderProjectSelect() {
  const select = $('#projectSelect');
  select.innerHTML = projects.map((p, i) =>
      `<option value="${i}" ${i === activeProject ? 'selected' : ''}>${esc(p.name)}</option>`)
      .join('');
  select.onchange = () => setActiveProject(+select.value);
  $('#removeProject').hidden = projects.length < 2;
}

function renderPageTabs() {
  renderProjectSelect();
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

$('#addProject').onclick = () => {
  projects.push(newProject(`Project ${projects.length + 1}`));
  setActiveProject(projects.length - 1);
};
$('#renameProject').onclick = () => {
  const name = prompt('Project name:', projects[activeProject].name);
  if (name) { projects[activeProject].name = name; renderProjectSelect(); persist(); }
};
$('#removeProject').onclick = () => {
  if (!confirm(`Remove project "${projects[activeProject].name}" and all its pages?`)) return;
  projects.splice(activeProject, 1);
  setActiveProject(Math.max(0, activeProject - 1));
};

async function rebuildAndSolve() {
  persist();
  await rpc('tableClear');
  // Project settings (productivity research levels etc.) apply to every
  // page solve; tableClear resets them, so re-send like the filler below.
  await rpc('tableSetSettings', JSON.stringify(projSettings()));
  const filler = pages[activePage].filler;
  if (filler) await rpc('tableSetFiller', JSON.stringify(filler));
  // Table units are per SECOND (desktop yafc compatible); UI shows /min.
  for (const goal of goals) {
    await rpc('tableAddLink', goal.tdn, goal.perMin / 60, linkAlgoOf(goal.tdn), goal.quality ?? '');
  }
  for (const entry of linked) {
    const { tdn, quality } = parseLinked(entry);
    await rpc('tableAddLink', tdn, 0, linkAlgoOf(entry), quality);
  }
  // Rows carry per-project choices ('' / missing = favorite-or-first
  // defaults): a reactor burning mox vs uranium cells is a different chain.
  for (const row of rows) {
    await rpc('tableAddRecipe', row.tdn, JSON.stringify({
      fixed: (row.fixed ?? 0) / 60,
      entity: row.entity ?? '',
      entityQuality: row.entityQuality ?? '',
      fuel: row.fuel ?? '',
      modules: row.modules,
      quality: row.quality ?? '',
    }));
  }
  renderSolve(await rpc('tableSolve'));
}

// ---- amount dialog: shared numeric entry for demand + pinned rate (native
// prompt() doesn't reliably appear in installed/standalone-mode PWAs on
// mobile, so both flows go through this in-page dialog instead).
function openAmountDialog({ title, hint, value, clearLabel, onApply, onClear, quality }) {
  $('#amountTitle').textContent = title;
  $('#amountHint').textContent = hint ?? '';
  const input = $('#amountInput');
  input.value = value ?? '';
  const clearBtn = $('#amountClear');
  clearBtn.hidden = !onClear;
  clearBtn.textContent = clearLabel ?? 'Clear';
  const qualityRow = $('#amountQualityRow');
  const qualitySelect = $('#amountQuality');
  qualityRow.hidden = !quality;
  if (quality) qualitySelect.innerHTML = qualitySelectHtml(quality.selected);
  const submit = () => {
    const qualityTdn = quality ? qualitySelect.value : undefined;
    if (onApply(parseFloat(input.value), input.value, qualityTdn)) $('#amountDialog').close();
  };
  $('#amountApply').onclick = submit;
  input.onkeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); submit(); } };
  clearBtn.onclick = () => { onClear(); $('#amountDialog').close(); };
  $('#amountCancel').onclick = () => $('#amountDialog').close();
  $('#amountDialog').showModal();
  input.focus();
  input.select();
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

  // Goals: amber pills, click amount to edit, x to remove. Main products
  // (output demand) get their own section, separate from input goals
  // (negative perMin — "consume this much").
  const goalPill = async (g) => {
    const i = goals.indexOf(g);
    return `<span class="pill goal${g.perMin < 0 ? ' input' : ''}">` +
        `<button class="chip" data-goods="${g.tdn}">${esc(g.name)}${await qualityBadge(qualityByTdn(g.quality))}</button>
           <button class="small ghost amt" data-goal="${i}" title="Edit demand">${g.perMin}/min</button>
           <button class="x" data-goal-x="${i}" aria-label="Remove goal">✕</button></span>`;
  };
  if (goals.length === 0) {
    $('#goals').innerHTML = '<span class="muted">No demand set — search a goods, then “Set demand”.</span>';
  } else {
    const outputs = goals.filter((g) => g.perMin >= 0);
    const inputs = goals.filter((g) => g.perMin < 0);
    const outputsHtml = (await Promise.all(outputs.map(goalPill))).join('');
    const inputsHtml = (await Promise.all(inputs.map(goalPill))).join('');
    $('#goals').innerHTML =
        (outputs.length ? `<div class="eyebrow" style="margin:0 0 4px">Main products</div>${outputsHtml}` : '') +
        (inputs.length ? `<div class="eyebrow" style="margin:10px 0 4px">Inputs to consume</div>${inputsHtml}` : '');
  }
  for (const btn of $('#goals').querySelectorAll('[data-goal]')) {
    btn.onclick = () => {
      const goal = goals[+btn.dataset.goal];
      openAmountDialog({
        title: `Demand for ${goal.name}`,
        hint: 'Units per minute — negative = input goal: consume this much.',
        value: goal.perMin,
        quality: hasSelectableQualities() ? { selected: goal.quality } : undefined,
        onApply: (perMin, raw, qualityTdn) => {
          if (!Number.isFinite(perMin) || perMin === 0) return false;
          goal.perMin = perMin;
          goal.quality = qualityTdn;
          rebuildAndSolve();
          return true;
        },
      });
    };
  }
  for (const btn of $('#goals').querySelectorAll('[data-goal-x]')) {
    btn.onclick = () => { goals.splice(+btn.dataset.goalX, 1); rebuildAndSolve(); };
  }

  // Recipe nameplates.
  const plates = await Promise.all(result.rows.map(async (row, i) => {
    const flows = (await Promise.all(row.flows.map(async (f) =>
        `<button class="flow chip ${f.perMin >= 0 ? 'pos' : 'neg'}" data-goods="${f.tdn}"` +
        ` title="${esc(f.tdn)}">${await iconImg(f.tdn)}${await qualityBadge(f.quality)}` +
        `${fmt(f.perMin * 60)}</button>`))).join('');
    const entity = row.entity && row.entity.tdn ? `<button class="entity chip" data-config="${i}"` +
        ` title="${esc(row.entity.locName)} — click to change building, fuel, modules">` +
        `${await iconImg(row.entity.tdn)}${await qualityBadge(row.entity.quality)}<span class="amt">×${fmt(row.buildings)}</span>` +
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
        <span class="name" title="${esc(row.recipe.tdn)}">${esc(row.recipe.locName)}${await qualityBadge(row.quality)}</span>
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
      openAmountDialog({
        title: 'Pin crafting rate',
        hint: 'Crafts per minute. Clear the field to let the solver choose the rate.',
        value: row.fixed ? String(row.fixed) : '',
        clearLabel: 'Unpin',
        onClear: row.fixed ? () => { delete row.fixed; rebuildAndSolve(); } : undefined,
        onApply: (value, raw) => {
          if (raw.trim() === '') { delete row.fixed; rebuildAndSolve(); return true; }
          if (!Number.isFinite(value) || value <= 0) return false;
          row.fixed = value;
          rebuildAndSolve();
          return true;
        },
      });
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
  const linkPills = await Promise.all(result.links.map(async (l) => {
    const qualityTdn = l.quality && l.quality.level > 0 ? l.quality.tdn : '';
    const key = linkedKey(l.tdn, qualityTdn);
    const isGoal = goals.some((g) => g.tdn === l.tdn && (g.quality ?? '') === qualityTdn);
    const bad = (l.flags & 1) !== 0;
    const algo = l.algo ?? 0;
    const unlink = isGoal ? '' :
        `<button class="x" data-unlink="${key}" aria-label="Unlink">✕</button>`;
    return `<span class="pill${bad ? ' bad' : ''}${algo ? ' loose' : ''}" title="${bad ? 'unmatched — needs both a producer and a consumer in-table' : 'matched'}">
        <button class="chip" data-goods="${l.tdn}">${esc(l.name)}${await qualityBadge(l.quality)}</button>
        <button class="small ghost algo" data-algo="${key}" title="${ALGO[algo].label}">${ALGO[algo].symbol}</button>
        <span class="amt">${fmt(l.flow * 60)}/min</span>${unlink}</span>`;
  }));
  $('#links').innerHTML = linkPills.join('') || '<span class="muted">—</span>';
  for (const btn of $('#links').querySelectorAll('[data-unlink]')) {
    btn.onclick = () => {
      pages[activePage].linked = linked.filter((entry) => entry !== btn.dataset.unlink);
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

  // Off-table flows with candidate auto-pull. Quality-tiered flows link at
  // their tier; the auto-pulled recipe row inherits that tier as its recipe
  // quality (a producer crafts at it, a consumer consumes at it).
  const flowRows = await Promise.all(result.flows
      .filter((f) => Math.abs(f.perMin) > 1e-9)
      .sort((a, b) => a.perMin - b.perMin)
      .map(async (f) => {
        const qualityTdn = f.quality && f.quality.level > 0 ? f.quality.tdn : '';
        return `<div class="row">${await iconImg(f.tdn)}` +
        `<span class="amt ${f.perMin > 0 ? 'pos' : 'neg'}">${fmt(f.perMin * 60)}/min</span>` +
        `<button class="chip" data-goods="${f.tdn}">${esc(f.name)}${await qualityBadge(f.quality)}</button>` +
        `<button class="small" data-pull="${f.tdn}" data-side="${f.perMin < 0 ? 'p' : 'c'}"` +
        ` data-quality="${qualityTdn}">${f.perMin < 0 ? 'produce ▸' : 'consume ▸'}</button>` +
        `<button class="small ghost" data-link="${linkedKey(f.tdn, qualityTdn)}">link only</button>` +
        `</div>`;
      }));
  $('#flows').innerHTML = flowRows.join('') || '<span class="muted">—</span>';
  for (const btn of $('#flows').querySelectorAll('[data-link]')) {
    btn.onclick = () => { linked.push(btn.dataset.link); rebuildAndSolve(); };
  }
  for (const btn of $('#flows').querySelectorAll('[data-pull]')) {
    btn.onclick = () => pullCandidates(btn.dataset.pull, btn.dataset.side === 'p',
                                       btn.dataset.quality);
  }
}

// ---- candidate auto-pull ----
const inTable = (tdn, qualityTdn = '') =>
    goals.some((g) => g.tdn === tdn && (g.quality ?? '') === qualityTdn) ||
    linked.includes(linkedKey(tdn, qualityTdn));

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

// qualityTdn '' = Normal. Pulling a non-Normal flow links that tier and sets
// the added row's recipe quality to it, so the new row's products (producer
// side) or ingredients (consumer side) bind to the tiered link.
async function pullCandidates(tdn, wantProducers, qualityTdn = '') {
  if (!inTable(tdn, qualityTdn)) linked.push(linkedKey(tdn, qualityTdn));
  const info = await rpc('goodsInfo', tdn);
  const list = wantProducers ? info.producers : info.consumers;
  // API pre-sorts: available first, then yafc cost ascending. Auto-pick never
  // grabs a recipe that is research- or milestone-locked, or a barreling/
  // voiding pseudo-recipe.
  const available = list.filter((r) =>
      r.available !== false && !r.milestone && !r.inaccessible && !r.special);
  if (available.length === 1) {
    rows.push({ tdn: available[0].tdn, quality: qualityTdn || undefined });
    await rebuildAndSolve();
    return;
  }
  await rebuildAndSolve();
  renderRecipeList(
      `${wantProducers ? 'Produce' : 'Consume'} ${info.goods.locName} — pick a recipe`,
      list, '', qualityTdn);
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

async function renderRecipeList(title, allEntries, header = '', rowQuality = '') {
  const list = reachable(allEntries);
  const hiddenNote = allEntries.length !== list.length
      ? `<div class="muted" style="font-size:12px;padding:2px 0">${allEntries.length - list.length} unreachable hidden (project settings)</div>`
      : '';
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
      `${header}<div class="eyebrow">${esc(title)}</div>` + hiddenNote + items.join('');
  for (const btn of $('#recipeInfo').querySelectorAll('[data-add]')) {
    btn.onclick = () => {
      rows.push({ tdn: btn.dataset.add, quality: rowQuality || undefined });
      rebuildAndSolve();
    };
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
  const results = reachable(await rpc('searchGoods', query, 20));
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
  // Fluids/Special goods never carry a quality tier (Factorio 2.0 rule), and
  // some packs disable the quality feature entirely (see hasSelectableQualities).
  const goodsAcceptsQuality = info.goods.kind !== 'Fluid' && info.goods.kind !== 'Special' &&
      hasSelectableQualities();
  $('#goalBtn').onclick = () => {
    openAmountDialog({
      title: `Demand for ${info.goods.locName}`,
      hint: 'Units per minute — negative = input goal: consume this much.',
      value: 900,
      quality: goodsAcceptsQuality ? { selected: undefined } : undefined,
      onApply: (perMin, raw, qualityTdn) => {
        if (!Number.isFinite(perMin) || perMin === 0) return false;
        goals.push({ tdn, name: info.goods.locName, perMin, quality: qualityTdn });
        rebuildAndSolve();
        return true;
      },
    });
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

// A .yafc file is one desktop project: importing replaces the ACTIVE web
// project's pages+settings (other projects in this bundle are untouched).
async function applyLoadedProject(state) {
  if (state.error) { status(`project load failed: ${state.error}`); return; }
  const project = projects[activeProject];
  project.pages = state.pages.map((p, i) => ({
    name: p.name || `Page ${i + 1}`,
    goals: p.goals.map((g) => ({ tdn: g.tdn, name: g.name, perMin: g.perMin, quality: g.quality })),
    linked: p.linked,
    rows: p.rows,          // rows keep entity/fuel/modules mirrors verbatim
    filler: p.filler,      // page-level module defaults, if any
    linkAlgos: p.linkAlgos ?? {},
  }));
  project.activePage = 0;
  project.settings = {
    miningProductivity: state.settings?.miningProductivity ?? 0,
    researchProductivity: state.settings?.researchProductivity ?? 0,
    productivityLevels: state.settings?.productivityLevels ?? {},
  };
  bindActivePage();
  renderPageTabs();
  if (state.errors?.length) status(`project loaded with ${state.errors.length} warnings`);
  await rebuildAndSolve();
}

async function importProjectText(text) {
  applyLoadedProject(await rpc('projectLoad', text));
}

// The active project as .yafc text ({pages, settings} shape — the C++ side
// also still accepts the older bare pages array).
const activeProjectSaveState = () =>
    JSON.stringify({ pages, settings: projSettings() });

$('#shareBtn').onclick = async () => {
  persist();
  const text = await rpc('projectSaveRaw', activeProjectSaveState());
  // Envelope carries which hosted pack to auto-load and the milestone set,
  // so opening the link on a fresh browser reproduces the table without
  // manual pack-picking or re-clicking through the milestones dialog.
  const envelope = JSON.stringify({
    pack: currentPackId,
    milestones: milestones.filter((m) => m.unlocked).map((m) => m.tdn),
    project: text,
  });
  const encoded = await deflateBase64Url(envelope);
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
  const text = await rpc('projectSaveRaw', activeProjectSaveState());
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

// {pack, milestones, project} envelope, or null if there's no shared link.
// Older links held the raw .yafc project text with no envelope — those
// still load, just without pack auto-select or milestone restore.
async function parseSharedPayload() {
  const encoded = sharedPayloadFromUrl();
  if (!encoded) return null;
  const decoded = await inflateBase64Url(encoded);
  try {
    const parsed = JSON.parse(decoded);
    if (parsed && typeof parsed === 'object' && typeof parsed.project === 'string') {
      return parsed;
    }
  } catch { /* not an envelope -> legacy raw project text */ }
  return { pack: null, milestones: null, project: decoded };
}

// Returns {ok, milestonesApplied} so the caller can skip its own
// milestones-restore step when the link already set them.
async function loadProjectFromUrl() {
  let shared;
  try {
    shared = await parseSharedPayload();
  } catch (error) {
    status(`shared project failed: ${error.message}`);
    return { ok: false, milestonesApplied: false };
  }
  if (!shared) return { ok: false, milestonesApplied: false };
  await importProjectText(shared.project);
  let milestonesApplied = false;
  if (Array.isArray(shared.milestones)) {
    milestones = await rpc('setMilestones', JSON.stringify({ unlocked: shared.milestones }));
    renderMilestonesSummary();
    if (bundleKey) {
      localStorage.setItem(bundleKey + ':milestones', JSON.stringify(shared.milestones));
    }
    milestonesApplied = true;
  }
  status('shared project loaded');
  return { ok: true, milestonesApplied };
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
    entityQuality: row.entityQuality ?? '',
    fuel: row.fuel ?? '',
    quality: row.quality ?? '',
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
         ${await iconImg(o.tdn)}<span>${esc(o.locName)}</span>${await lockBadge(o)}
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

  // Two quality dimensions per row, both hidden when the loaded pack has no
  // real quality tiers (hasSelectableQualities): "recipe quality" is the tier
  // the craft targets (ingredients consume at it; products spread upward from
  // it when quality modules are slotted); "building quality" scales the
  // machine itself (+30% crafting speed per level, same power draw).
  const qualityPicker = async (kind, selectedTdn) =>
      (await Promise.all(reachable(qualities).map(async (q) =>
      `<button class="opt${(selectedTdn || qualities[0]?.tdn) === q.tdn ? ' sel' : ''}"
         data-${kind}="${q.tdn}">
         ${await iconImg(q.tdn)}<span>${esc(q.locName)}</span>
         <span class="meta">${q.inaccessible ? 'unreachable' : q.milestone ? 'locked' : ''}</span>
       </button>`))).join('');
  const qualityOpts = await qualityPicker('pick-quality', rowConfig.quality);
  const entityQualityOpts = await qualityPicker('pick-entity-quality', rowConfig.entityQuality);

  $('#rowDialogBody').innerHTML = `
    <div class="eyebrow">Building</div><div class="opts">${crafters}</div>
    ${hasSelectableQualities() ? `
      <div class="eyebrow" title="+30% crafting speed per level, unchanged power draw">Building quality</div>
      <div class="opts">${entityQualityOpts}</div>` : ''}
    ${opts.hasEnergy && opts.fuels.length ? `<div class="eyebrow">Fuel</div><div class="opts">${fuels}</div>` : ''}
    ${hasSelectableQualities() ? `
      <div class="eyebrow" title="Ingredients are consumed at this tier; products start at it and upgrade from quality modules">Recipe quality</div>
      <div class="opts">${qualityOpts}</div>` : ''}
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
  for (const btn of body.querySelectorAll('[data-pick-quality]')) {
    btn.onclick = () => { rowConfig.quality = btn.dataset.pickQuality; renderRowConfig(); };
  }
  for (const btn of body.querySelectorAll('[data-pick-entity-quality]')) {
    btn.onclick = () => {
      rowConfig.entityQuality = btn.dataset.pickEntityQuality;
      renderRowConfig();
    };
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
    row.entityQuality = rowConfig.entityQuality || undefined;
    row.fuel = rowConfig.fuel || undefined;
    row.quality = rowConfig.quality || undefined;
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
// ---- quality tiers (Normal/Uncommon/Rare/.../Legendary in vanilla) ----
// [{tdn, locName, level, inaccessible?, milestone?}], level-ordered; fetched
// once per bundle and re-fetched when the milestone set changes (gating is
// milestone-based, same as recipes/goods).
let qualities = [];
const qualityLocked = (q) => q.inaccessible ? ' \u{1F6AB}' : q.milestone ? ' \u{1F512}' : '';
// Some packs disable the quality feature entirely at the game-data level
// (a common overhaul-mod choice) — the loaded Database then only carries
// Normal plus Factorio's internal "quality-unknown" sentinel (itself always
// inaccessible, never a real player-facing tier). Gate the whole feature's
// UI on there being at least one REAL, reachable non-Normal tier, so those
// packs don't show a picker whose only alternative is a dead-end sentinel.
const hasSelectableQualities = () => qualities.some((q) => q.level > 0 && !q.inaccessible);
async function refreshQualities() {
  qualities = await rpc('qualityList');
}
function qualitySelectHtml(selectedTdn) {
  return reachable(qualities).map((q) =>
      `<option value="${q.tdn}" ${q.tdn === selectedTdn ? 'selected' : ''}>` +
      `${esc(q.locName)}${qualityLocked(q)}</option>`).join('');
}

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
  // The search hint tracks milestone progression: the next pack you haven't
  // reached is what you're probably hunting recipes for.
  const next = milestones.find((m) => !m.unlocked) ?? milestones.at(-1);
  $('#search').placeholder = next
      ? `Search goods — try ${next.locName}` : 'Search goods…';
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

// ---- project settings: productivity research (mining % / research % /
// per-technology levels). Per project — quality recycling loop math changes
// dramatically with productivity research, so each project models one
// research state. Fractions in state (matching .yafc), percent in the UI.
let prodTechOptions = null;  // [{tdn, locName, bonusPerLevel, recipes}] per bundle

async function openProjectSettings() {
  prodTechOptions ??= await rpc('productivityOptions');
  const settings = projSettings();
  $('#setHideUnreachable').checked = !!settings.hideUnreachable;
  $('#setMiningProd').value = Math.round((settings.miningProductivity ?? 0) * 100);
  $('#setResearchProd').value = Math.round((settings.researchProductivity ?? 0) * 100);
  // The unreachable filter applies here too (a hidden tech keeps its saved
  // level — the Done handler folds unseen inputs back from current settings).
  const rowsHtml = await Promise.all(reachable(prodTechOptions).map(async (t) => {
    const level = settings.productivityLevels?.[t.tdn] ?? 0;
    const affected = (t.recipes ?? []).map((r) => r.locName).join(', ');
    return `<div class="row">${await iconImg(t.tdn)}
       <span style="flex:1" title="${esc(affected)}">${esc(t.locName)}${await lockBadge(t)}</span>
       <span class="muted mono">+${Math.round(t.bonusPerLevel * 100)}%/lv</span>
       <input type="number" min="0" step="1" value="${level}" data-prod-tech="${t.tdn}"
              aria-label="Research level">
     </div>`;
  }));
  $('#prodTechList').innerHTML = rowsHtml.join('') ||
      '<div class="muted">This pack has no per-recipe productivity researches.</div>';
  $('#projectSettingsDialog').showModal();
}

$('#projectSettingsBtn').onclick = openProjectSettings;
$('#projectSettingsDone').onclick = () => {
  const settings = projSettings();
  settings.hideUnreachable = $('#setHideUnreachable').checked;
  settings.miningProductivity = Math.max(0, (+$('#setMiningProd').value || 0) / 100);
  settings.researchProductivity = Math.max(0, (+$('#setResearchProd').value || 0) / 100);
  // Start from saved levels so techs hidden from the list (unreachable
  // filter) keep theirs; visible inputs then overwrite/remove.
  const levels = { ...(settings.productivityLevels ?? {}) };
  for (const input of $('#prodTechList').querySelectorAll('[data-prod-tech]')) {
    const level = Math.max(0, Math.round(+input.value || 0));
    if (level > 0) levels[input.dataset.prodTech] = level;
    else delete levels[input.dataset.prodTech];
  }
  settings.productivityLevels = levels;
  $('#projectSettingsDialog').close();
  // The catalog panel may be showing now-filtered content.
  const search = $('#search');
  if (search.value.trim().length >= 2) search.oninput({ target: search });
  rebuildAndSolve();
};

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
  refreshQualities();
  prodTechOptions = null;  // milestone lock badges in the list may be stale
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
let packNames = {};  // manifest id -> display name, for the header label

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
  for (const p of manifest.packs) packNames[p.id] = p.name;

  // A shared link names the pack it was built with (human priority: opening
  // a link should never require guessing which pack to click) — it wins
  // over the last-used pack so a link always reproduces on a fresh browser.
  let shared = null;
  try {
    shared = await parseSharedPayload();
  } catch { /* handled again, with a visible status message, in loadProjectFromUrl */ }
  const wanted = shared?.pack || localStorage.getItem('mancos:lastPack');

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
  const defaultPack = manifest.packs.find((p) => p.id === wanted);
  if (defaultPack) {
    loadPack(defaultPack.id, defaultPack.file);
  } else if (shared?.pack) {
    status(`shared link needs pack "${shared.pack}", not available here — ` +
           `pick a pack manually, or load a local bundle built from the same mods`);
  }
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
  currentPackId = packId;
  if (packId) localStorage.setItem('mancos:lastPack', packId);
  // Header label next to "Switch pack": manifest display name for hosted
  // packs, the file name for locally loaded bundles.
  const packLabel = (packId && packNames[packId]) || label || '';
  $('#packName').textContent = packLabel;
  $('#packName').title = packLabel;
  status(`${info.objects} objects · ${info.recipes} recipes · factorio ${info.meta.factorioVersion}`);
  bundleKey = 'mancos:' + JSON.stringify(info.meta.mods ?? label);
  // History is per-bundle: undoing into another pack's pages would resolve
  // against the wrong database.
  undoStack.length = 0;
  redoStack.length = 0;
  lastProjectsJson = null;
  lastSnapshot = null;
  prodTechOptions = null;  // per-bundle: refetched on next settings open
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
  const { ok: fromUrl, milestonesApplied: sharedMilestonesApplied } = await loadProjectFromUrl();
  if (!fromUrl) {
    if (restore()) {
      rebuildAndSolve();
    } else {
      // Fresh pack with no saved project: start clean — never carry the
      // previous pack's projects into this bundle's state.
      projects = [newProject('Project 1')];
      activeProject = 0;
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
  // Skipped entirely when a shared link already set the milestone set above.
  if (!sharedMilestonesApplied) {
    let savedMilestones = null;
    try {
      savedMilestones = JSON.parse(localStorage.getItem(bundleKey + ':milestones'));
    } catch { /* treat as first visit */ }
    milestones = await rpc('setMilestones',
                           JSON.stringify({ unlocked: savedMilestones ?? [] }));
    renderMilestonesSummary();
    if (!Array.isArray(savedMilestones) && milestones.length > 0) openMilestones();
  }
  // After milestones (shared-link or local-restore path above already ran
  // EnsureMilestones in the worker) so this doesn't trigger the accessibility
  // walk any earlier than the existing deferred-milestones ordering intends.
  await refreshQualities();
}

// ---- switch pack: close the current project, back to the chooser ----
// The project is already persisted per bundle on every change; picking this
// pack again later restores it exactly. Clearing lastPack means a reload
// also lands on the chooser instead of auto-loading.
$('#switchBtn').onclick = () => {
  persist();
  localStorage.removeItem('mancos:lastPack');
  bundleKey = null;
  projects = [newProject('Project 1')];
  activeProject = 0;
  bindActivePage();
  undoStack.length = 0;
  redoStack.length = 0;
  lastProjectsJson = null;
  lastSnapshot = null;
  milestones = [];
  qualities = [];
  prodTechOptions = null;
  favorites = new Set();
  for (const id of ['shareBtn', 'exportBtn', 'importBtn', 'switchBtn']) {
    document.getElementById(id).disabled = true;
  }
  $('#search').disabled = true;
  $('#search').value = '';
  $('#search').placeholder = 'Search goods…';
  $('#results').innerHTML = '';
  $('#recipeInfo').innerHTML = '';
  $('#techResults').innerHTML = '';
  $('#milestonesSummary').textContent = '';
  updateUndoButtons();
  $('#workspace').hidden = true;
  $('#dropHint').hidden = false;
  $('#packName').textContent = '';
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

// ---- app mode: installable + offline via the service worker ----
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./sw.js').catch(() => { /* http dev */ });
}
let installPrompt = null;
window.addEventListener('beforeinstallprompt', (e) => {
  e.preventDefault();
  installPrompt = e;
  $('#installBtn').hidden = false;
});
$('#installBtn').onclick = async () => {
  if (!installPrompt) return;
  installPrompt.prompt();
  await installPrompt.userChoice;
  installPrompt = null;
  $('#installBtn').hidden = true;
};

initPacks();
$('#clearBtn').onclick = () => {
  pages[activePage] = newPage(pages[activePage].name);
  bindActivePage();
  rebuildAndSolve();
};

// ---- blueprint export: the current page's solved rows as a stampable
// Factorio blueprint (sets of buildings with recipes/modules/fuel set) ----
$('#blueprintBtn').onclick = async () => {
  const name = `${projects[activeProject].name} — ${pages[activePage].name}`;
  const result = await rpc('tableExportBlueprint', JSON.stringify({ label: name }));
  if (result.error) { status(`blueprint: ${result.error}`); return; }
  const summary = `${result.buildings} buildings, ${result.width}×${result.height} tiles` +
      (result.truncatedRows ? ` — ${result.truncatedRows} row(s) capped at 200 buildings` : '');
  try {
    await navigator.clipboard.writeText(result.blueprint);
    status(`blueprint copied — ${summary}`);
  } catch {
    prompt(`Blueprint (${summary}):`, result.blueprint);
  }
};
