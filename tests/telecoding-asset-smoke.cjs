'use strict';
// Preserve browser syntax, essential diagnostics entry points and original
// read-only ECU schemas when minifying the gzip-served telecoding asset.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const cp = require('node:child_process');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root,'data/telecoding.html'), 'utf8');
const python = process.platform === 'win32' ? 'python' : 'python3';
const processResult = cp.spawnSync(python, ['scripts/minify_html_js.py','data/telecoding.html'], {
  cwd:root,encoding:'utf8',maxBuffer:2*1024*1024
});
assert.equal(processResult.status,0,processResult.stderr);
const optimized = processResult.stdout;
function body(html) {
  const m=html.match(/<script\b[^>]*>([\s\S]*?)<\/script\s*>/i);
  assert.ok(m,'Inline script available'); return m[1];
}
new vm.Script(body(source),{filename:'telecoding-original.js'});
new vm.Script(body(optimized),{filename:'telecoding-minified.js'});
// No public entrypoint/function names or diagnostic semantics should disappear.
const names=[
  'parsePsaKwpCompactDtcResponse','parseKwpTwoByteDtcWithStatusResponse',
  'parseUds1902DtcResponse','renderFaultsResponse',
  'runDiagnosticTest','readAllZones','writeModifiedZones','loadECU'
];
for(const name of names){
  assert.match(source,new RegExp('\\b'+name+'\\b'),'Source contains '+name);
  assert.match(optimized,new RegExp('\\b'+name+'\\b'),'Optimized contains '+name);
}
const before=body(source), after=body(optimized);
// Only inline JS may differ: source and optimized HTML must otherwise match.
assert.equal(source.replace(before,'__SCRIPT__'),optimized.replace(after,'__SCRIPT__'));
assert.ok(after.length<before.length,'Minified JS should be smaller');
console.log('PASS telecoding source '+source.length+' bytes -> optimized '+optimized.length+
  ' bytes; both JS scripts parse; public diagnostic entrypoints preserved.');
