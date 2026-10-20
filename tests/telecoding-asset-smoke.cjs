'use strict';
// Preserve browser syntax, essential diagnostics entry points and original
// read-only ECU schemas when minifying the gzip-served telecoding asset.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const cp = require('node:child_process');
const root = path.resolve(__dirname, '..');
const pageNames = ['telecoding.html','index.html','dashboard.html','carstate.html','monitor.html','media.html'];
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
for(const filename of pageNames.filter(name=>name!=='telecoding.html')) {
  const originalHtml=fs.readFileSync(path.join(root,'data',filename),'utf8');
  const result=cp.spawnSync(python,['scripts/minify_html_js.py','data/'+filename],{
    cwd:root,encoding:'utf8',maxBuffer:2*1024*1024
  });
  assert.equal(result.status,0,filename+': '+result.stderr);
  const optimizedHtml=result.stdout;
  const originalScript=body(originalHtml);
  const optimizedScript=body(optimizedHtml);
  new vm.Script(originalScript,{filename:filename+':source'});
  new vm.Script(optimizedScript,{filename:filename+':compressed'});
  assert.equal(originalHtml.replace(originalScript,'__JS__'),
    optimizedHtml.replace(optimizedScript,'__JS__'),filename+' HTML preserved');
  assert.ok(optimizedScript.length<originalScript.length,filename+' minification saved bytes');
}
console.log('PASS all '+pageNames.length+' static JS assets parse before/after compression.');

