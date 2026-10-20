// Run: node tests/web-media-smoke.cjs
// Zero dependencies. Exercises the embedded browser script without a car or Spotify account.
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const { webcrypto } = require("node:crypto");

const { execFileSync } = require("node:child_process");
const rootDir = path.join(__dirname, "..");
const python = process.platform === "win32" ? "python" : "python3";
// Exercise the exact optimized JavaScript the ESP32 serves, not just the
// original readable data/media.html source.
const header = execFileSync(python, ["scripts/minify_html_js.py", "data/media.html"], {
  cwd: rootDir, encoding: "utf8", maxBuffer: 1024*1024
});
const match = header.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(match, "Expected media HTML browser script");
const code = match[1];
new vm.Script(code);

const els = new Map();
function element(id) {
  if (!els.has(id)) els.set(id, {
    value: "", checked: false, textContent: "", style: {},
    addEventListener() {}, matches() { return false; },
    removeAttribute() {}, replaceChildren() {},
  });
  return els.get(id);
}
function memoryStorage() {
  const values = new Map();
  return {
    getItem: k => values.get(k) || null,
    setItem: (k, v) => values.set(k, String(v)),
    removeItem: k => values.delete(k),
  };
}
const documentListeners = new Map();
const intervalDelays = [];
const timeoutDelays = [];
const documents = {
  hidden: false,
  getElementById: element,
  addEventListener(type, callback) {
    if (!documentListeners.has(type)) documentListeners.set(type, []);
    documentListeners.get(type).push(callback);
  }
};
const tokens = memoryStorage();
const local = memoryStorage();
let simulated = [
  { next_count: 0, previous_count: 0, seek_up: false, seek_down: false },
  { next_count: 1, previous_count: 0, seek_up: true, seek_down: false },
  { next_count: 1, previous_count: 0, seek_up: true, seek_down: false },
  { next_count: 1, previous_count: 1, seek_up: false, seek_down: true },
];
let offline = false;
async function fakeFetch(url, opts={}) {
  if (String(url).includes("/api/spotify/status")) {
    return {ok:true, json:async()=>({ready:true, configured:false,enabled:false,online:false,last_http:0,last_action:"",error:""})};
  }
  if (String(url).includes("/api/spotify/")) {
    return {ok:true, json:async()=>({configured:true,enabled:!!opts.body&&JSON.parse(opts.body).enabled===true})};
  }
  if (offline) throw new Error("Simulated Wi-Fi outage");
  return { ok: true, json: async () => simulated.shift() || {next_count:1,previous_count:1} };
}
const context = vm.createContext({
  location: { origin: "http://192.168.100.1", href: "" },
  document: documents,
  localStorage: local,
  sessionStorage: tokens,
  fetch: fakeFetch,
  setInterval: (_fn, ms) => { intervalDelays.push(ms); return intervalDelays.length; },
  setTimeout: (_fn, ms) => { timeoutDelays.push(ms); return timeoutDelays.length; },
  clearTimeout: () => {},
  crypto: webcrypto,
  AbortController,
  URL,
  URLSearchParams,
  btoa,
  console
});

const settle = () => new Promise(resolve => setImmediate(resolve));
async function main() {
  vm.runInContext(code, context);
  await settle(); // Initial read starts automatically
  assert.equal(element("counts").textContent, "0 / 0", "Initial counters");
  assert.equal(vm.runInContext("base64url(sha256Ascii('abc'))", context), "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0", "SHA-256 test vector");
  assert.equal(vm.runInContext("base64url(sha256Ascii('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk'))",context), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC 7636 PKCE test vector");

  for (let i=0;i<3;i++) { await vm.runInContext("pollBridge()",context); await settle(); }
  assert.equal(element("counts").textContent, "1 / 1");
  assert.equal(element("lastEvent").textContent, "Previous ⏮");
  const lines = element("log").textContent.split("\n").filter(line => line.includes("Stalk event:"));
  assert.equal(lines.length, 2, "Two edges, not three repeated frames");
  assert.equal(element("active").checked, false, "Stalk forwarding off by default");
  await vm.runInContext("refreshStandalone()",context);
  assert.equal(element("standaloneState").textContent,"Not installed","No stored secrets in test fixture");
  // Background controller disables the browser forwarding path to prevent duplicate commands.
  await vm.runInContext(`(() => { const oldFetch = fetch; fetch = async (url) => String(url).includes("/api/spotify/status") ?
    ({ok:true,json:async()=>({configured:true,enabled:true,online:true,last_http:204,last_action:"next",error:""})}) : oldFetch(url);
    return refreshStandalone().finally(()=>{fetch=oldFetch;}); })()`,context);
  assert.equal(element("active").disabled,true,"No double control when ESP32 is autonomous");
  assert.equal(element("active").checked,false,"Autonomous control unchecks browser forwarding");

  offline = true;
  await vm.runInContext("pollBridge()",context);await settle();
  offline = false;
  simulated = [{next_count:10,previous_count:12}];
  await vm.runInContext("pollBridge()",context);await settle();
  assert.equal(element("counts").textContent, "10 / 12", "Reconnection updates counters");
  assert.equal(element("log").textContent.split("\n").filter(line=>line.includes("Stalk event:")).length,2,"No stale events replayed after reconnection");

  tokens.setItem("psa-spotify-pkce-v1", JSON.stringify({
    verifier:"x",state:"legitimate",clientId:"a".repeat(32),redirectUri:"https://example.org/callback",created:Date.now()
  }));
  element("callbackValue").value="https://example.org/callback?code=thecode&state=attacker";
  await assert.rejects(vm.runInContext("finishAuth()",context),/OAuth state mismatch/,"CSRF state protection");

  // Switching between dashboard and media must never create extra status timers.
  assert.equal(intervalDelays.filter(ms => ms === 5000).length, 1, "Exactly one status timer after boot");
  const visibilityCallbacks = documentListeners.get("visibilitychange") || [];
  assert.equal(visibilityCallbacks.length, 1, "Single visibility listener");
  for(let i=0;i<5;i++) {
    documents.hidden = true;
    visibilityCallbacks[0]();
    documents.hidden = false;
    visibilityCallbacks[0]();
    await settle();
  }
  assert.equal(intervalDelays.filter(ms => ms === 5000).length, 1,
    "Foreground/background switches cannot multiply status requests");

  // When ESP32 handles SEEK autonomously, browser HTTP polling should slow down.
  await vm.runInContext("pollLoop()", context);
  await settle();
  assert.ok(timeoutDelays.includes(2000), "Autonomous media polling uses a 2s interval");
  console.log("PASS: browser syntax, SHA-256 + PKCE, stalk edges, reconnect, OAuth state, one status timer, low-rate autonomous polling.");
}
main().catch(error => { console.error(error); process.exitCode=1; });
