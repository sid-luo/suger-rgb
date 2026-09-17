import assert from 'node:assert/strict';
import { readFile, realpath } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import test from 'node:test';

const installer = new URL('../docs/installer/', import.meta.url);
const html = await readFile(new URL('index.html', installer), 'utf8');
const entry = new URL(html.match(/<script type="module" src="([^"]+)"/)[1], installer);
const dialog = new URL('install-dialog-im156JnI.js', entry);
const source = await readFile(dialog, 'utf8');
const method = source.slice(source.indexOf('async _confirmInstall(){'), source.indexOf('async _doProvision()'));

// Follow static, side-effect, and dynamic imports as URLs, as a browser does.
test('the complete module graph has one URL per file and no cache query strings', async () => {
  const seen = new Set();
  const identities = new Map();
  async function visit(url) {
    if (seen.has(url.href)) return;
    seen.add(url.href);
    assert.equal(url.search, '', `query changes module identity: ${url}`);
    const path = await realpath(fileURLToPath(url));
    assert.ok(!identities.has(path) || identities.get(path) === url.href, `duplicate identity: ${url}`);
    identities.set(path, url.href);
    const code = await readFile(url, 'utf8');
    for (const [, specifier] of code.matchAll(/(?:\bfrom\s*|\bimport\s*(?:\(\s*)?)["']([^"']+)["']/g)) {
      await visit(new URL(specifier, url));
    }
  }
  await visit(entry);
  assert.ok(seen.size >= 25, `only visited ${seen.size} modules`);
  assert.ok(seen.has(dialog.href));
});

const deferred = () => {
  let resolve;
  const promise = new Promise(r => { resolve = r; });
  return { promise, resolve };
};
const tick = () => new Promise(resolve => setImmediate(resolve));

function fixture({ failAt, resetGate, disconnectGate, resetFails, disconnectFails, closeFails, reopenFails, locked, downloadFails } = {}) {
  const events = [];
  let opens = 0;
  const port = {
    readable: {}, writable: {},
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    async open(options) {
      assert.equal(this.readable, null, 'attempted to open an already open port');
      if (reopenFails && opens === 1) throw new Error('reopen failed');
      opens++;
      events.push(`open:${opens}`);
      this.readable = {}; this.writable = {};
    },
    async close() {
      events.push('close');
      if (closeFails && opens === 1) throw new Error('close failed');
      this.readable = null; this.writable = null;
    }
  };
  class Transport {
    constructor(device) { this.device = device; }
    async setRTS() { events.push('reset:start'); if (resetFails) throw new Error('reset failed'); }
    async disconnect() {
      events.push('disconnect:start');
      if (disconnectGate) await disconnectGate.promise;
      if (disconnectFails) {
        if (locked) this.device.readable.locked = true;
        throw new Error('disconnect failed');
      }
      await this.device.close();
      events.push('disconnect:end');
    }
  }
  class Loader {
    constructor({ transport, baudrate }) { this.transport = transport; assert.equal(baudrate, 115200); this.chip = { CHIP_NAME: failAt === 'unsupported' ? 'ESP32' : 'ESP32-C3' }; }
    async main(mode) {
      assert.equal(mode, 'usb_reset');
      await this.transport.device.open({ baudRate: 115200 });
      events.push('main');
      if (failAt === 'initialize') throw Object.assign(new Error('duplicate module registration'), { name: 'NotSupportedError' });
    }
    async flashId() { if (failAt === 'flashId') throw new Error('flash ID failed'); }
    async after() { if (resetGate) await resetGate.promise; events.push('reset:end'); }
    async eraseFlash() { events.push('erase'); if (failAt === 'erase') throw new Error('erase failed'); }
    async writeFlash(options) {
      events.push('write');
      if (failAt === 'write') throw new Error('write failed');
      assert.equal(options.compress, true);
      assert.equal(options.eraseAll, false);
      assert.equal(options.fileArray[0].address, 0);
      options.reportProgress(0, 4, 4);
    }
  }
  class Reader {
    addEventListener(_, fn) { this.onLoad = fn; }
    readAsArrayBuffer(blob) { this.result = new Uint8Array([1, 2, 3, 4]).buffer; this.onLoad(); }
  }
  const context = vm.createContext({
    qr: Transport, Es: Loader, window: {}, URL, Uint8Array, FileReader: Reader,
    location: { toString: () => 'https://example.test/installer/' },
    fetch: async () => ({ ok: !downloadFails, status: 503, blob: async () => ({}) }),
    xe: async () => {},
    console: { error: (...args) => events.push('caught'), warn: (...args) => events.push('warning'), log: () => {} },
  });
  const reset = source.match(/wa=async\(e,t\)=>\{[^}]+\}/)[0];
  vm.runInContext(`var ${reset};`, context);
  const confirm = vm.runInContext(`({${method}})._confirmInstall`, context);
  const states = [];
  const ui = {
    port, _client: null, _zh: true, _state: 'INSTALL', _installErase: true,
    manifestPath: 'manifest.json',
    _manifest: { builds: [{ chipFamily: 'ESP32-C3', parts: [{ path: 'firmware/test.bin', offset: 0 }] }] },
    async _initialize(value) { assert.equal(value, true); events.push('initialize:app'); this._client = null; },
    requestUpdate() { events.push('render'); },
    set _installState(value) { states.push(value); this.lastState = value; },
    get _installState() { return this.lastState; },
  };
  return { ui, port, events, states, run: () => confirm.call(ui) };
}

for (const failAt of ['initialize', 'flashId', 'unsupported', 'write', undefined]) {
  test(`${failAt ?? 'success'} waits for reset AND disconnect before reconnecting`, async () => {
    const resetGate = deferred(), disconnectGate = deferred();
    const f = fixture({ failAt, resetGate, disconnectGate });
    const running = f.run();
    await tick();
    assert.equal(f.events.filter(e => e.startsWith('open:')).length, 1);
    assert.ok(!f.states.some(s => s?.state === 'error' || s?.state === 'finished'));
    await f.run(); // Ignore a second click while cleanup is still running.
    assert.equal(f.events.filter(e => e === 'main').length, 1);
    resetGate.resolve();
    await tick();
    assert.ok(f.events.includes('disconnect:start'));
    assert.ok(!f.events.includes('open:2'));
    disconnectGate.resolve();
    await running;
    assert.ok(f.events.indexOf('disconnect:end') < f.events.indexOf('open:2'));
    assert.equal(f.ui.lastState.state, failAt ? 'error' : 'finished');
    assert.equal(f.ui._installRunning, false);
    if (failAt === 'initialize') assert.match(f.ui.lastState.message, /NotSupportedError: duplicate module registration/);
    if (!failAt) assert.ok(f.events.indexOf('open:2') < f.events.indexOf('initialize:app'));
  });
}

test('a failed reset still releases the transport and recovers the original failure', async () => {
  const f = fixture({ failAt: 'initialize', resetFails: true });
  await f.run();
  assert.ok(f.events.includes('disconnect:end'));
  assert.ok(f.events.includes('open:2'));
  assert.equal(f.ui.lastState.details.error, 'failed_initialize');
});

test('a failed transport disconnect still closes an unlocked port', async () => {
  const f = fixture({ failAt: 'initialize', disconnectFails: true });
  await f.run();
  assert.ok(f.events.includes('open:2'));
  assert.equal(f.ui.lastState.details.error, 'failed_initialize');
});

for (const options of [{ reopenFails: true }, { closeFails: true }, { disconnectFails: true, locked: true }, { failAt: 'erase' }]) {
  test(`session errors are caught and offer reconnect: ${JSON.stringify(options)}`, async () => {
    const f = fixture(options);
    await assert.doesNotReject(f.run());
    assert.equal(f.ui._state, 'ERROR');
    assert.match(f.ui._error, /关闭对话框后重新连接/);
    assert.equal(f.ui._installRunning, false);
    assert.ok(!f.events.includes('open:2'));
    if (options.failAt === 'erase') assert.equal(f.port.readable, null);
  });
}

test('firmware download failure cleans up and preserves its error', async () => {
  const f = fixture({ downloadFails: true });
  await f.run();
  assert.equal(f.ui.lastState.details.error, 'failed_firmware_download');
  assert.ok(f.events.includes('disconnect:end'));
  assert.ok(!f.events.includes('write'));
});

test('the actual transport releases its writer when serial write rejects', async () => {
  const code = source.slice(source.indexOf('class qr{'), source.indexOf('function Gr('));
  const Transport = vm.runInNewContext(`${code};qr`, { Uint8Array });
  let released = false;
  const transport = new Transport({
    writable: { getWriter: () => ({
      async write() { throw new Error('USB write failed'); },
      releaseLock() { released = true; },
    }) },
  });
  await assert.rejects(transport.write(new Uint8Array([1])), /USB write failed/);
  assert.equal(released, true, 'disconnect would otherwise wait forever on the writer lock');
});
