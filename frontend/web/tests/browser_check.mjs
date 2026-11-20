// browser_check.mjs — friend-local.html in a real browser, opened the way
// most people will open it: double-clicked, from a file. A headless
// Chromium on a profile of its own, driven over the DevTools protocol,
// with mock_gemini.mjs standing in for the provider:
//
//   node browser_check.mjs <chromium> build/friend-local.html
//
// It checks the page starts, keeps things in the browser's storage,
// makes a friend, has a conversation with it, hears it through a
// microphone (Chromium's fake one, which beeps), and still has all of
// that after a reload, with nothing in the console along the way.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const [browser, pagePath] = process.argv.slice(2);
const page = resolve(pagePath);
const here = dirname(fileURLToPath(import.meta.url));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const MOCK_PORT = 8766, DEBUG_PORT = 9334;

let failed = 0, checks = 0;
const check = (ok, what, got) => {
  checks++;
  if (!ok) { failed++; console.log('  FAIL ' + what + (got !== undefined ? ': ' + JSON.stringify(got) : '')); }
};

const mock = spawn(process.execPath, [join(here, 'mock_gemini.mjs'), page, String(MOCK_PORT)], { stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 'friend-local-browser-'));
const chrome = spawn(browser, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  // A fake microphone, allowed without asking, and sound without a tap
  // to start it, since nobody here can tap.
  '--no-sandbox', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
  '--autoplay-policy=no-user-gesture-required',
  `--user-data-dir=${profile}`, `--remote-debugging-port=${DEBUG_PORT}`, 'about:blank'],
{ stdio: 'ignore' });
const done = (code) => { chrome.kill(); mock.kill(); try { rmSync(profile, { recursive: true, force: true }); } catch (e) { /* busy */ } process.exit(code); };

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
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') problems.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
  // What the browser itself complains about, like a worklet it would not
  // load, which the page's own console never sees.
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') problems.push(m.params.entry.text);
});
const send = (method, params = {}) => new Promise((r) => { const id = ++nextId; waiting.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
const run = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: `(async () => { ${expr} })()`, awaitPromise: true, returnByValue: true });
  if (r.result.exceptionDetails) return { threw: r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text };
  return r.result.result.value;
};
const open = async () => {
  await send('Page.navigate', { url: 'file://' + page });
  for (let i = 0; i < 50; i++) { await sleep(200); if (await run('return !!(window.W && document.title)')) break; }
  await sleep(500);
};
await send('Runtime.enable');
await send('Log.enable');
await send('Page.enable');

await open();
const first = await run(`return { lasting: Store.lasting, trouble: document.getElementById('trouble').hidden ? '' : document.getElementById('trouble-text').textContent, welcome: !document.getElementById('welcome').hidden };`);
check(first.lasting === true, 'the browser keeps things for a page opened from a file', first);
check(first.trouble === '', 'it starts without complaint', first.trouble);
check(first.welcome === true, 'with nobody in it, it says hello', first);

const talk = await run(`
  Settings.v.keys[0] = 'test-key';
  Settings.v.bases[0] = 'http://localhost:${MOCK_PORT}';
  await Settings.store();
  applyConfig();
  W.web_reconfigure();
  await createFriend('Juniper');
  document.getElementById('text').value = 'hello from a file';
  Composer.send();
  for (let i = 0; i < 100; i++) {
    await new Promise((r) => setTimeout(r, 100));
    const st = stats();
    if (W.web_message_count() >= 2 && st.settled && !st.unsaved && !S.saving) break;
  }
  await saveNow(true);
  const n = W.web_message_count();
  return { n, reply: n ? readMessage(n - 1).text : '', trouble: document.getElementById('trouble').hidden ? '' : document.getElementById('trouble-text').textContent };
`);
check(talk.n === 2, 'a message and its reply', talk);
check(/hello from a file/.test(talk.reply || ''), 'the reply came from the provider', talk.reply);

const voice = await run(`
  await Voice.toggle();
  const seen = [];
  let level = 0;
  for (let i = 0; i < 80; i++) {
    await new Promise((r) => setTimeout(r, 100));
    const v = W.web_voice_state();
    level = Math.max(level, W.web_voice_level());
    if (seen[seen.length - 1] !== v) seen.push(v);
    if (v === 2) break;
  }
  const open = Mic.on_;
  const node = Mic.node && Mic.node.constructor.name, ctx = Sound.ctx && Sound.ctx.state;
  Voice.stop();
  return { seen, level, node, ctx, open, closed: !Mic.on_, trouble: document.getElementById('trouble').hidden ? '' : document.getElementById('trouble-text').textContent };
`);
check(voice.open, 'the microphone opens', voice);
check(voice.seen.includes(2), 'and what it hears reaches the friend', voice);
check(voice.closed, 'and closes again', voice);

await open();
const after = await run(`return { current: S.entry && S.entry.name, n: W.web_message_count(), key: Settings.key(), rows: document.querySelectorAll('.friend-row').length };`);
check(after.current === 'Juniper', 'after a reload the friend is still there', after);
check(after.n === 2, 'and so is the conversation', after);
check(after.key === 'test-key', 'and the settings', after.key);
check(problems.length === 0, 'nothing went wrong in the page', problems);

console.log(`${checks} checks, ${failed} failed`);
ws.close();
done(failed ? 1 : 0);
