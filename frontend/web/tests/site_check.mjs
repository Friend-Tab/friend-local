// site_check.mjs — the GitHub Pages site (./build.sh site) in a headless
// Chromium: served from a web address, the browser must count it as an
// app it can install, its offline helper must take over, and with the
// network gone it must still open, with what was kept still in it.
//
//   node site_check.mjs <chromium> build/site
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const [browser, siteDir] = process.argv.slice(2);
const here = dirname(fileURLToPath(import.meta.url));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const PORT = 8768, DEBUG_PORT = 9336;
const url = `http://localhost:${PORT}/`;

let failed = 0, checks = 0;
const check = (ok, what, got) => {
  checks++;
  if (!ok) { failed++; console.log('  FAIL ' + what + (got !== undefined ? ': ' + JSON.stringify(got) : '')); }
};

const server = spawn(process.execPath, [join(here, 'mock_gemini.mjs'), resolve(siteDir), String(PORT)], { stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 'friend-local-site-'));
const chrome = spawn(browser, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--no-sandbox', `--user-data-dir=${profile}`, `--remote-debugging-port=${DEBUG_PORT}`, 'about:blank'], { stdio: 'ignore' });
const done = (code) => { chrome.kill(); server.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch (e) { /* busy */ } process.exit(code); };

let targets = [];
for (let i = 0; i < 100 && !targets.length; i++) {
  try { targets = (await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json`)).json()).filter((t) => t.type === 'page'); } catch (e) { /* starting */ }
  if (!targets.length) await sleep(200);
}
if (!targets.length) { console.log('the browser did not start'); done(1); }
const ws = new WebSocket(targets[0].webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let nextId = 0;
const waiting = new Map(), problems = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') problems.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error' && !/ERR_INTERNET_DISCONNECTED/.test(m.params.entry.text)) problems.push(m.params.entry.text);
});
const send = (method, params = {}) => new Promise((r) => { const id = ++nextId; waiting.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
const run = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: `(async () => { ${expr} })()`, awaitPromise: true, returnByValue: true });
  return r.result.exceptionDetails ? { threw: r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text } : r.result.result.value;
};
const open = async () => {
  await send('Page.navigate', { url });
  for (let i = 0; i < 50; i++) { await sleep(200); if (await run('return !!(window.W && document.title)')) break; }
  await sleep(500);
};
await send('Runtime.enable');
await send('Log.enable');
await send('Page.enable');
await send('Network.enable');

await open();
const sw = await run(`
  const reg = await navigator.serviceWorker.ready;
  for (let i = 0; i < 50 && !navigator.serviceWorker.controller; i++) await new Promise((r) => setTimeout(r, 100));
  return { active: !!reg.active, controlled: !!navigator.serviceWorker.controller, kept: (await caches.keys()).length };
`);
check(sw.active, 'the offline helper starts', sw);
const manifest = await send('Page.getAppManifest');
check(!manifest.result.errors?.length && /friend/.test(manifest.result.data || ''), 'the manifest reads', manifest.result.errors);
const install = await send('Page.getInstallabilityErrors');
check(install.result && install.result.installabilityErrors.length === 0, 'the browser would install it',
  install.result && install.result.installabilityErrors);

// Something to find again: a friend, saved.
await open(); // now under the helper
const made = await run(`await createFriend('Wren'); await saveNow(true); return (await Store.friends()).map((f) => f.name);`);
check(Array.isArray(made) && made.includes('Wren'), 'a friend is made', made);

await send('Network.emulateNetworkConditions', { offline: true, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
server.kill();
await open();
const offline = await run(`return { started: !!W, current: S.entry && S.entry.name, trouble: document.getElementById('trouble').hidden ? '' : document.getElementById('trouble-text').textContent };`);
check(offline.started && offline.current === 'Wren', 'with no network it still opens, with the friend in it', offline);
check(problems.length === 0, 'nothing went wrong in the page', problems);

console.log(`${checks} checks, ${failed} failed`);
ws.close();
done(failed ? 1 : 0);
