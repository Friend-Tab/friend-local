// mock_gemini.mjs — a stand-in for the Gemini API, for trying the page
// without a key or a bill. It serves friend-local.html too, so the page
// and the "provider" are on the same machine:
//
//   node frontend/web/tests/mock_gemini.mjs build/friend-local.html 8765
//   node frontend/web/tests/mock_gemini.mjs build/site 8765
//
// then open http://localhost:8765/ and set Settings › Advanced › Base URL
// to http://localhost:8765 with the key "test-key". Replies stream back a
// few words at a time; a spoken reply is a short tone. Every request is
// logged, without its key.
import { createServer } from 'node:http';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, extname } from 'node:path';

const page = process.argv[2] || 'build/friend-local.html';
const port = Number(process.argv[3] || 8765);
const KEY = 'test-key';
// A folder (./build.sh site's) is served as a site; a file as the page.
const siteDir = statSync(page).isDirectory() ? page : null;

const cors = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'POST, GET, OPTIONS',
  'Access-Control-Allow-Headers': 'content-type, x-goog-api-key, authorization',
};

function lastUserText(body) {
  const contents = body.contents || [];
  for (let i = contents.length - 1; i >= 0; i--) {
    if (contents[i].role === 'model') continue;
    const parts = contents[i].parts || [];
    const text = parts.filter((p) => p.text).map((p) => p.text).join(' ');
    const photos = parts.filter((p) => p.inlineData && /image/.test(p.inlineData.mimeType)).length;
    return { text, photos };
  }
  return { text: '', photos: 0 };
}

function tone(seconds, rate) {
  const n = Math.floor(seconds * rate);
  const pcm = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const fade = Math.min(1, i / 400, (n - i) / 400);
    pcm.writeInt16LE(Math.round(Math.sin((i / rate) * 2 * Math.PI * 440) * 8000 * fade), i * 2);
  }
  return pcm.toString('base64');
}

function embedding(text) {
  let h = 2166136261;
  for (const c of text) h = Math.imul(h ^ c.charCodeAt(0), 16777619);
  const v = [];
  for (let i = 0; i < 768; i++) { h = Math.imul(h ^ i, 16777619); v.push(((h >>> 0) / 4294967296) - 0.5); }
  return v;
}

const counts = {};
createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (req.method === 'OPTIONS') { res.writeHead(204, cors); res.end(); return; }
  if (req.method === 'GET') {
    const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
    const name = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
    if (siteDir && /^[\w.-]+$/.test(name) && existsSync(join(siteDir, name))) {
      res.writeHead(200, { 'Content-Type': types[extname(name)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
      res.end(readFileSync(join(siteDir, name)));
    } else if (!siteDir && (url.pathname === '/' || url.pathname.endsWith('.html'))) {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(readFileSync(page));
    } else if (url.pathname === '/counts') {
      res.writeHead(200, { 'Content-Type': 'application/json', ...cors });
      res.end(JSON.stringify(counts));
    } else { res.writeHead(404); res.end(); }
    return;
  }
  let raw = '';
  req.on('data', (c) => { raw += c; });
  req.on('end', async () => {
    const verb = url.pathname.split(':').pop();
    const model = (url.pathname.match(/models\/([^:]+)/) || [])[1];
    counts[verb] = (counts[verb] || 0) + 1;
    console.log(new Date().toISOString().slice(11, 19), req.method, model, verb, raw.length + ' bytes');
    if (req.headers['x-goog-api-key'] !== KEY) {
      res.writeHead(400, { 'Content-Type': 'application/json', ...cors });
      res.end(JSON.stringify({ error: { code: 400, message: 'API key not valid. Please pass a valid API key.', status: 'INVALID_ARGUMENT' } }));
      return;
    }
    let body = {};
    try { body = JSON.parse(raw); } catch (e) { /* not JSON */ }
    if (verb === 'embedContent') {
      const text = ((body.content || {}).parts || []).map((p) => p.text).join(' ');
      res.writeHead(200, { 'Content-Type': 'application/json', ...cors });
      res.end(JSON.stringify({ embedding: { values: embedding(text) } }));
      return;
    }
    if (verb === 'streamGenerateContent') {
      const { text, photos } = lastUserText(body);
      const reply = photos ? `nice photo. you said "${text.trim()}"` : `you said "${text.trim()}". tell me more`;
      res.writeHead(200, { 'Content-Type': 'text/event-stream', ...cors });
      const words = reply.split(' ');
      for (let i = 0; i < words.length; i += 2) {
        const piece = words.slice(i, i + 2).join(' ') + (i + 2 < words.length ? ' ' : '');
        res.write('data: ' + JSON.stringify({ candidates: [{ content: { role: 'model', parts: [{ text: piece }] } }] }) + '\r\n\r\n');
        await new Promise((r) => setTimeout(r, 150));
      }
      res.end();
      return;
    }
    // generateContent: speech, transcription, or one of the friend's
    // own thinking calls, told apart by what was asked for.
    const config = body.generationConfig || {};
    const parts = (body.contents || []).flatMap((c) => c.parts || []);
    let out;
    if ((config.responseModalities || []).includes('AUDIO')) {
      out = { inlineData: { mimeType: 'audio/L16;codec=pcm;rate=24000', data: tone(0.6, 24000) } };
    } else if (parts.some((p) => p.inlineData && /audio/.test(p.inlineData.mimeType))) {
      out = { text: 'hello from the microphone' };
    } else if (parts.some((p) => p.inlineData && /image/.test(p.inlineData.mimeType))) {
      out = { text: 'a photograph of something colourful' };
    } else {
      out = { text: '{}' };
    }
    res.writeHead(200, { 'Content-Type': 'application/json', ...cors });
    res.end(JSON.stringify({ candidates: [{ content: { role: 'model', parts: [out] } }] }));
  });
}).listen(port, () => console.log(`page and pretend Gemini on http://localhost:${port}/ (key: ${KEY})`));
