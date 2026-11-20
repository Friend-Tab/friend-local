/* host.js — what the WebAssembly module imports from the page: logging
 * and the maths and number formatting it has no C library for. Shared by
 * the page and by tests/run.mjs, which runs the library's own tests in
 * WebAssembly. Defines friendHostImports(getMemory, log). */

/* printf's %e, %f and %g, as C writes them: exponents of at least two
 * digits, %g without trailing zeros. JavaScript rounds correctly, which
 * is what makes this worth doing here rather than in C. */
function friendFormatNumber(v, precision, style) {
  if (Number.isNaN(v)) return 'nan';
  if (!Number.isFinite(v)) return v < 0 ? '-inf' : 'inf';
  const negative = v < 0 || Object.is(v, -0);
  const a = Math.abs(v);
  const exp = (x, p) => {
    const [m, e] = x.toExponential(p).split('e');
    const n = Number(e);
    return m + 'e' + (n < 0 ? '-' : '+') + String(Math.abs(n)).padStart(2, '0');
  };
  const fixed = (x, p) => (x < 1e21 ? x.toFixed(p) : exp(x, p));
  let out;
  if (style === 0) out = exp(a, precision);
  else if (style === 1) out = fixed(a, precision);
  else {
    const p = precision === 0 ? 1 : precision;
    const x = a === 0 ? 0 : Number(a.toExponential(p - 1).split('e')[1]);
    if (x < p && x >= -4) {
      out = fixed(a, p - 1 - x);
      if (out.includes('.')) out = out.replace(/0+$/, '').replace(/\.$/, '');
    } else {
      out = exp(a, p - 1);
      out = out.replace(/(\.\d*?)0+e/, '$1e').replace(/\.e/, 'e');
    }
  }
  return (negative ? '-' : '') + out;
}

function friendHostImports(getMemory, log) {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  return {
    host: {
      log(ptr, len) {
        log(decoder.decode(new Uint8Array(getMemory().buffer, ptr, len)));
      },
      format_number(value, precision, style, out, cap) {
        const bytes = encoder.encode(friendFormatNumber(value, precision, style));
        const n = Math.min(bytes.length, cap);
        new Uint8Array(getMemory().buffer, out, n).set(bytes.subarray(0, n));
        return n;
      },
      pow: Math.pow,
      exp: Math.exp,
      log_e: Math.log,
    },
  };
}

if (typeof module !== 'undefined') module.exports = { friendHostImports, friendFormatNumber };
