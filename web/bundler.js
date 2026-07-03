// The bundler page (main thread, DOM only) — the filesystem-facing half of
// the split app (PLAN Phase 4.5). Grants itself temporary, page-session-only
// read access to the user's local Factorio installation via the File System
// Access API, then hands the picked directory handles to bundler-worker.js
// (which runs the wasm core and never touches the main thread). The main
// planner app (index.html) never reads game files — it only loads the
// .yafcbundle this page produces.
const $ = (sel) => document.querySelector(sel);
const esc = (s) => s.replace(/[&<>"]/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

let dataHandle = null;
let modsHandle = null;
let worker = null;

function supported() {
  return typeof window.showDirectoryPicker === 'function';
}

function updateButtons() {
  $('#buildBtn').disabled = dataHandle == null;
}

function log(line) {
  const el = $('#log');
  el.textContent += (el.textContent ? '\n' : '') + line;
  el.scrollTop = el.scrollHeight;
}

async function pickFolder(kind) {
  try {
    const handle = await window.showDirectoryPicker({ id: `mancos-${kind}`, mode: 'read' });
    if (kind === 'data') {
      dataHandle = handle;
      $('#dataFolder').textContent = handle.name;
    } else {
      modsHandle = handle;
      $('#modsFolder').textContent = handle.name;
      $('#modsClear').hidden = false;
    }
    updateButtons();
  } catch (e) {
    if (e.name !== 'AbortError') log(`folder pick failed: ${e.message}`);
  }
}

$('#dataBtn').onclick = () => pickFolder('data');
$('#modsBtn').onclick = () => pickFolder('mods');
$('#modsClear').onclick = () => {
  modsHandle = null;
  $('#modsFolder').textContent = '(vanilla — no mods folder)';
  $('#modsClear').hidden = true;
};

function fmtBytes(n) {
  if (n >= 1e6) return (n / 1e6).toFixed(1) + ' MB';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + ' KB';
  return n + ' B';
}

async function saveBundle(bytes, suggestedName) {
  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  if (typeof window.showSaveFilePicker === 'function') {
    try {
      const handle = await window.showSaveFilePicker({
        suggestedName,
        types: [{ description: 'yafc bundle', accept: { 'application/octet-stream': ['.yafcbundle'] } }],
      });
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
      return;
    } catch (e) {
      if (e.name === 'AbortError') return;
      log(`save-as failed, falling back to download: ${e.message}`);
    }
  }
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = suggestedName;
  a.click();
  URL.revokeObjectURL(url);
}

$('#buildBtn').onclick = async () => {
  if (dataHandle == null) return;
  $('#buildBtn').disabled = true;
  $('#dataBtn').disabled = true;
  $('#modsBtn').disabled = true;
  $('#log').textContent = '';
  $('#result').hidden = true;
  $('#progressWrap').hidden = false;

  // A fresh worker per run: a pipeline failure leaves the wasm instance
  // aborted and unusable (see bundler-worker.js), and a clean slate also
  // avoids any WORKERFS mount state leaking between attempts.
  worker?.terminate();
  worker = new Worker('./bundler-worker.js', { type: 'module' });
  worker.onmessage = async ({ data }) => {
    if (data.progress) {
      log(data.progress);
      return;
    }
    $('#buildBtn').disabled = false;
    $('#dataBtn').disabled = false;
    $('#modsBtn').disabled = false;
    if (data.error) {
      log(`FAILED: ${data.error}`);
      return;
    }
    const { result: stats, bundle } = data;
    log('done.');
    const mods = Object.keys(stats.mods ?? {}).filter((m) => m !== 'core' && m !== 'base');
    const name = mods.length ? `${mods[0]}${mods.length > 1 ? `+${mods.length - 1}` : ''}` : 'vanilla';
    const suggested = `${name}-${stats.factorioVersion}.yafcbundle`;

    $('#result').hidden = false;
    $('#result').innerHTML = `
      <div class="eyebrow">Bundle ready</div>
      <div class="row"><span class="muted">objects</span><span class="mono">${stats.objects}</span></div>
      <div class="row"><span class="muted">database</span><span class="mono">${fmtBytes(stats.databaseBytes)}</span></div>
      <div class="row"><span class="muted">icons</span><span class="mono">${stats.iconFiles} files, ${fmtBytes(stats.iconBytes)}${stats.missingIcons ? ` (${stats.missingIcons} unresolved)` : ''}</span></div>
      <div class="row"><span class="muted">languages</span><span class="mono">${stats.languages}</span></div>
      <div class="row"><span class="muted">mods</span><span class="mono">${Object.keys(stats.mods ?? {}).length}</span></div>
      <div class="row"><span class="muted">factorio</span><span class="mono">${esc(stats.factorioVersion)}</span></div>
      <div class="row"><span class="muted">total size</span><span class="mono">${fmtBytes(stats.bundleBytes)}</span></div>
      ${stats.warnings?.length ? `<details style="margin-top:6px">
        <summary class="muted" style="cursor:pointer">${stats.warnings.length} warnings (recoverable — bundle still built)</summary>
        <div class="mono muted" style="font-size:12px;white-space:pre-wrap;margin-top:6px">${esc(stats.warnings.join('\n'))}</div>
      </details>` : ''}
      <button id="saveBtn" style="margin-top:10px">Save ${esc(suggested)}</button>
    `;
    $('#saveBtn').onclick = () => saveBundle(bundle, suggested);
  };
  worker.postMessage({ method: 'run', dataHandle, modsHandle });
};

if (!supported()) {
  $('#unsupported').hidden = false;
  $('#picker').hidden = true;
}

// Chrome refuses folder access to OS-protected locations (on Windows that
// includes Program Files, where Factorio usually lives): the picker either
// blocks the folder or shows it greyed out. Tell users where their install
// probably is and to copy it somewhere ordinary first.
(() => {
  const platform = (navigator.userAgentData?.platform ?? navigator.platform ?? '')
      .toLowerCase();
  const paths = platform.includes('win') ? {
    os: 'Windows',
    data: ['C:\\Program Files (x86)\\Steam\\steamapps\\common\\Factorio\\data (Steam)',
           'C:\\Program Files\\Factorio\\data (standalone)'],
    mods: ['%APPDATA%\\Factorio\\mods'],
    copyTo: 'C:\\Users\\<you>\\Documents\\factorio-bundle\\',
    blocked: true,
  } : platform.includes('mac') ? {
    os: 'macOS',
    data: ['~/Library/Application Support/Steam/steamapps/common/factorio/factorio.app/Contents/data'],
    mods: ['~/Library/Application Support/factorio/mods'],
    copyTo: '~/Documents/factorio-bundle/',
    blocked: false,
  } : {
    os: 'Linux',
    data: ['~/.local/share/Steam/steamapps/common/Factorio/data',
           '~/.steam/steam/steamapps/common/Factorio/data'],
    mods: ['~/.factorio/mods'],
    copyTo: '~/factorio-bundle/',
    blocked: false,
  };
  const list = (items) => items.map((p) => `<code>${esc(p)}</code>`).join('<br>');
  $('#dirAccessNote').innerHTML =
      `<strong>Where to find these on ${paths.os}:</strong><br>` +
      `Data: ${list(paths.data)}<br>Mods: ${list(paths.mods)}<br>` +
      (paths.blocked
          ? `<strong>Note:</strong> Chrome treats <code>Program Files</code> and other ` +
            `system locations as protected and won't let this page read them. If the ` +
            `picker refuses your install folder, copy <code>data/</code> (and ` +
            `<code>mods/</code>) to somewhere like <code>${esc(paths.copyTo)}</code> ` +
            `and pick the copies instead.`
          : `If the picker refuses a folder (some system locations are protected), ` +
            `copy <code>data/</code> and <code>mods/</code> to e.g. ` +
            `<code>${esc(paths.copyTo)}</code> and pick the copies.`);
})();
