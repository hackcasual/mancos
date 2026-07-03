// Dedicated worker hosting the wasm core (threading directive: the core never
// runs on the main thread). Message protocol: {id, method, args} in,
// {id, result} | {id, error} out. iconFile returns a transferred ArrayBuffer.
import createYafcModule from './yafc_web.js';

const modulePromise = createYafcModule();

self.onmessage = async (event) => {
  const { id, method, args } = event.data;
  try {
    const mod = await modulePromise;
    let result;
    switch (method) {
      case 'loadBundle': {
        // args[0]: ArrayBuffer of the bundle file -> copy into wasm memory.
        const bytes = new Uint8Array(args[0]);
        const ptr = mod._malloc(bytes.length);
        mod.HEAPU8.set(bytes, ptr);
        try {
          result = JSON.parse(mod.loadBundlePtr(ptr, bytes.length));
        } finally {
          mod._free(ptr);
        }
        break;
      }
      case 'iconFile': {
        const view = mod.iconFile(args[0]);
        if (view === null) {
          result = null;
        } else {
          const copy = new Uint8Array(view.length);
          copy.set(view);
          self.postMessage({ id, result: copy.buffer }, [copy.buffer]);
          return;
        }
        break;
      }
      default: {
        const raw = mod[method](...args);
        result = typeof raw === 'string' ? JSON.parse(raw) : raw;
      }
    }
    self.postMessage({ id, result });
  } catch (error) {
    self.postMessage({ id, error: String(error) });
  }
};
