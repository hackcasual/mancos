// Injected into the generated module factory scope (emcc --pre-js), so it
// can see the bare `FS`/`WORKERFS` runtime symbols without exporting them
// on the Module object. Exposes a minimal mount API for bundler-worker.js:
// the user's picked directories arrive there as arrays of File objects
// (relative paths baked into File.name — see bundler-worker.js) and are
// mounted read-only, lazily read via WORKERFS's FileReaderSync.
Module['mountFS'] = function (mountpoint, files) {
  try {
    FS.unmount(mountpoint);
  } catch (e) {
    // not mounted yet
  }
  FS.mkdirTree(mountpoint);
  FS.mount(WORKERFS, { files: files }, mountpoint);
};
