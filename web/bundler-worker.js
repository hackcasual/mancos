// Dedicated worker hosting the bundler's wasm core (same threading directive
// as web/worker.js: the core never runs on the main thread). The bundler
// page (bundler.html/bundler.js) transfers the FileSystemDirectoryHandles it
// got from showDirectoryPicker() here — handles are structured-cloneable, so
// the (possibly tens of thousands of files) directory walk happens in this
// worker instead of doubling as a huge postMessage payload of File objects.
//
// Protocol: {method:'run', dataHandle, modsHandle} in. Out, unsolicited as
// they happen: {progress} messages, then exactly one of {result} | {error}.
// On success, result.bundle is a transferred ArrayBuffer of the .yafcbundle.
import createMancosBundlerModule from './mancos_bundler_web.js';

const modulePromise = createMancosBundlerModule();

// Walks a picked directory into a flat array of File objects whose .name is
// the path relative to the directory root — WORKERFS builds its tree from
// File.name (see libworkerfs.js), so this is how nested mod folders arrive
// as nested paths under the mount point. Re-wrapping via `new File([file],
// path)` only changes the name; it does not read the underlying bytes (read
// happens lazily, per WORKERFS chunk, via FileReaderSync).
async function collectFiles(dirHandle, prefix = '') {
  const out = [];
  for await (const [name, handle] of dirHandle.entries()) {
    const path = prefix ? `${prefix}/${name}` : name;
    if (handle.kind === 'directory') {
      out.push(...(await collectFiles(handle, path)));
    } else {
      const file = await handle.getFile();
      out.push(new File([file], path, { type: file.type, lastModified: file.lastModified }));
    }
  }
  return out;
}

self.onmessage = async (event) => {
  const { method, dataHandle, modsHandle } = event.data;
  if (method !== 'run') return;
  const progress = (step) => self.postMessage({ progress: step });
  try {
    const mod = await modulePromise;

    progress('reading data folder…');
    const dataFiles = await collectFiles(dataHandle);
    mod.mountFS('/gamedata', dataFiles);

    let modsPath = '';
    if (modsHandle) {
      progress('reading mods folder…');
      const modFiles = await collectFiles(modsHandle);
      mod.mountFS('/mods', modFiles);
      modsPath = '/mods';
    }

    const statsJson = mod.runBundler('/gamedata', modsPath, progress);
    const stats = JSON.parse(statsJson);
    if (stats.error) {
      self.postMessage({ error: stats.error });
      return;
    }

    const view = mod.bundleBytes();
    const bytes = new Uint8Array(view.length);
    bytes.set(view);
    self.postMessage({ result: stats, bundle: bytes.buffer }, [bytes.buffer]);
  } catch (error) {
    // A pipeline error that made it here (rather than being caught and
    // returned as stats.error) means the wasm module aborted — it cannot be
    // reused. The page respawns a fresh worker on the next attempt.
    self.postMessage({ error: String(error?.message ?? error) });
  }
};
