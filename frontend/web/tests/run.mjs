// run.mjs — runs a WebAssembly build of the library's tests in node.
// node run.mjs build/web_tests.wasm
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const { friendHostImports, friendFormatNumber } =
  createRequire(import.meta.url)('../host.js');

// The formatting the page does for printf, against what C does.
const cases = [
  [0, 9, 2, '0'], [1, 9, 2, '1'], [0.5, 9, 2, '0.5'], [-2.25, 9, 2, '-2.25'],
  [1e-7, 9, 2, '1e-07'], [123456789012, 9, 2, '1.23456789e+11'],
  [0.1, 9, 2, '0.1'], [3.14159265358979, 9, 2, '3.14159265'],
  [100000, 9, 2, '100000'], [1e9, 9, 2, '1e+09'], [0.0001, 9, 2, '0.0001'],
  [0.00001, 9, 2, '1e-05'], [-0, 9, 2, '-0'], [1.5, 2, 1, '1.50'],
  [12345.678, 3, 0, '1.235e+04'], [0.30000001192092896, 9, 2, '0.300000012'],
];
let bad = 0;
for (const [v, p, s, want] of cases) {
  const got = friendFormatNumber(v, p, s);
  if (got !== want) { console.log(`format ${v} %.${p}${'efg'[s]}: got ${got}, want ${want}`); bad++; }
}

let memory;
let out = '';
const imports = friendHostImports(() => memory, (text) => { out += text; process.stdout.write(text); });
imports.env = {};
const { instance } = await WebAssembly.instantiate(readFileSync(process.argv[2]), imports);
memory = instance.exports.memory;
const code = instance.exports.main ? instance.exports.main() : instance.exports.__main_void();
if (bad) console.log(`${bad} formatting cases wrong`);
process.exit(code || bad ? 1 : 0);
