// Main-thread UI (DOM only). The wasm core lives in worker.js; everything
// here is postMessage RPC + rendering. Milestone 1: load bundle, search
// goods, add demands/links/recipes, solve, inspect flows, first-layer icons.

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

// ---- icons: first layer only for now (compositing comes later) ----
const iconUrlCache = new Map();
async function iconUrl(tdn) {
  if (iconUrlCache.has(tdn)) return iconUrlCache.get(tdn);
  const promise = (async () => {
    const layers = await rpc('iconLayers', tdn);
    if (!Array.isArray(layers) || layers.length === 0) return null;
    const bytes = await rpc('iconFile', layers[0].file);
    if (!bytes) return null;
    return URL.createObjectURL(new Blob([bytes], { type: 'image/png' }));
  })();
  iconUrlCache.set(tdn, promise);
  return promise;
}
async function iconImg(tdn) {
  const url = await iconUrl(tdn);
  return url ? `<img src="${url}" alt="">` : '';
}

// ---- state ----
const goals = [];     // {tdn, name, perMin}
const linked = [];    // tdn list (amount 0)
const rows = [];      // {tdn, name}

async function rebuildAndSolve() {
  await rpc('tableClear');
  for (const goal of goals) await rpc('tableAddLink', goal.tdn, goal.perMin);
  for (const tdn of linked) await rpc('tableAddLink', tdn, 0);
  for (const row of rows) await rpc('tableAddRecipe', row.tdn);
  const result = await rpc('tableSolve');
  renderSolve(result);
}

function fmt(x) {
  const abs = Math.abs(x);
  if (abs >= 1000) return (x / 1000).toFixed(2) + 'k';
  return abs >= 100 ? x.toFixed(1) : x.toFixed(2);
}

async function renderSolve(result) {
  const statusNames = ['solved', 'no solution (deadlocks unexplained)',
                       'numerical errors', 'unexpected error'];
  status(`${statusNames[result.status] ?? '?'} — ${rows.length} rows`);

  const tbody = $('#rows tbody');
  $('#rows').hidden = rows.length === 0;
  tbody.innerHTML = '';
  for (const row of result.rows) {
    const flows = row.flows
        .map((f) => `<span class="${f.perMin >= 0 ? 'pos' : 'neg'}">` +
                    `${fmt(f.perMin)} ${f.tdn.split('.')[1]}</span>`)
        .join(' &nbsp; ');
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${await iconImg(row.recipe.tdn)}</td>` +
                   `<td>${row.recipe.locName}${row.warnings ? ' ⚠️' : ''}</td>` +
                   `<td class="num">${fmt(row.craftsPerMin)}</td><td>${flows}</td>`;
    tbody.append(tr);
  }

  $('#links').innerHTML = result.links.map((l) =>
      `<span class="pill">${l.name}: ${fmt(l.flow)}/min` +
      `${l.flags & 1 ? ' (unmatched)' : ''}</span>`).join(' ') || '—';

  const flowRows = await Promise.all(result.flows
      .filter((f) => Math.abs(f.perMin) > 1e-9)
      .map(async (f) =>
      `<div class="row">${await iconImg(f.tdn)}<span class="${f.perMin > 0 ? 'pos' : 'neg'}">` +
      `${fmt(f.perMin)}/min</span> ${f.name} ` +
      `<button class="small ghost" data-link="${f.tdn}">link</button></div>`));
  $('#flows').innerHTML = flowRows.join('') || '—';
  for (const btn of $('#flows').querySelectorAll('button[data-link]')) {
    btn.onclick = () => { linked.push(btn.dataset.link); rebuildAndSolve(); };
  }

  $('#goals').innerHTML = goals.map((g) =>
      `<span class="pill">${g.name}: ${g.perMin}/min</span>`).join(' ') ||
      'No goals yet — search a goods and set a demand.';
}

// ---- search / info ----
$('#search').oninput = async (e) => {
  const query = e.target.value.trim();
  if (query.length < 2) { $('#results').innerHTML = ''; return; }
  const results = await rpc('searchGoods', query, 20);
  const html = await Promise.all(results.map(async (g) =>
      `<div class="row goods" data-tdn="${g.tdn}" style="cursor:pointer">` +
      `${await iconImg(g.tdn)}<span>${g.locName}</span>` +
      `<span class="muted">${g.kind}</span></div>`));
  $('#results').innerHTML = html.join('');
  for (const el of $('#results').querySelectorAll('[data-tdn]')) {
    el.onclick = () => showGoods(el.dataset.tdn);
  }
};

async function showGoods(tdn) {
  const info = await rpc('goodsInfo', tdn);
  const goal = `<button id="goalBtn">Set demand…</button>`;
  const recipes = await Promise.all(info.producers.map(async (r, i) =>
      `<div class="row">${await iconImg(r.tdn)}<span title="${r.tdn}">${r.locName}</span>` +
      `<span class="muted">${r.time}s</span>` +
      `<button class="small" data-add="${r.tdn}">+ row</button></div>` +
      `<div class="muted" style="margin-left:32px">` +
      `${r.in.map((x) => `${x.amount} ${x.name}`).join(', ') || 'no inputs'} → ` +
      `${r.out.map((x) => `${x.amount} ${x.name}`).join(', ')}</div>`));
  $('#recipeInfo').innerHTML =
      `<h2>${info.goods.locName} ${goal}</h2>` +
      `<h2>Producers (${info.producers.length})</h2>` + recipes.join('');
  $('#goalBtn').onclick = () => {
    const perMin = parseFloat(prompt(`Demand for ${info.goods.locName} (per minute):`, '900'));
    if (Number.isFinite(perMin) && perMin > 0) {
      goals.push({ tdn, name: info.goods.locName, perMin });
      rebuildAndSolve();
    }
  };
  for (const btn of $('#recipeInfo').querySelectorAll('button[data-add]')) {
    btn.onclick = () => {
      rows.push({ tdn: btn.dataset.add });
      rebuildAndSolve();
    };
  }
}

// ---- bundle loading ----
$('#loadBtn').onclick = () => $('#bundleFile').click();
$('#bundleFile').onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  status(`loading ${file.name} (${(file.size / 1e6).toFixed(1)} MB)…`);
  const buffer = await file.arrayBuffer();
  const info = await rpc('loadBundle', buffer);
  if (info.error) { status(`load failed: ${info.error}`); return; }
  status(`${file.name}: ${info.objects} objects, ${info.recipes} recipes, ` +
         `factorio ${info.meta.factorioVersion}`);
  $('#search').disabled = false;
  $('#solveBtn').disabled = false;
  $('#clearBtn').disabled = false;
};
$('#solveBtn').onclick = rebuildAndSolve;
$('#clearBtn').onclick = () => {
  goals.length = linked.length = rows.length = 0;
  rebuildAndSolve();
};
