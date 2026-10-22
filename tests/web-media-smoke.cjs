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
const pendingTimeouts = new Map();
let nextTimeoutId = 0;
function mediaTimers() {
  return [...pendingTimeouts.values()].filter(t => t.delay === 400 || t.delay === 2000);
}
async function fireMediaTimer() {
  const entry = [...pendingTimeouts.entries()].find(([, t]) => t.delay === 400 || t.delay === 2000);
  assert.ok(entry, "Media timer must be scheduled");
  pendingTimeouts.delete(entry[0]);
  entry[1].callback();
  await settle();
}
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
let simulatedStandalone = {ready:true,configured:false,enabled:false,online:false,
  last_http:0,last_action:"",error:""};
async function fakeFetch(url, opts={}) {
  if (String(url).includes("/api/oem/status")) {
    const payload=new Uint32Array(36);
    payload[0]=0x4F454D31;payload[11]=1;
    payload[12]=5000;payload[13]=0x05000004;
    return {ok:true,arrayBuffer:async()=>payload.buffer};
  }
  if (String(url).includes("/api/oem/display-test")) {
    return {ok:true,status:200,text:async()=>"acknowledged"};
  }
  if (String(url).includes("/api/spotify/status")) {
    return {ok:true, json:async()=>({...simulatedStandalone})};
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
  setTimeout: (callback, delay) => {
    const id = ++nextTimeoutId;
    pendingTimeouts.set(id, {callback, delay});
    return id;
  },
  clearTimeout: (id) => { pendingTimeouts.delete(id); },
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
  assert.match(element("oemTrace").textContent,/SOURCE 165/,
    "OEM status must not consume media event counters");
  await vm.runInContext('oemPost("display-test",{mode:0})',context);
  assert.equal(element("oemResult").textContent.startsWith("Stopped"),true,
    "Repeat stop uses independent OEM endpoint");
  await assert.rejects(
    Promise.resolve().then(()=>vm.runInContext('oemConfirm("Send")',context)),
    /Confirm the shared-bus risk/,
    "Unacknowledged OEM transmit remains blocked");
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
  assert.equal(element("radioSourceState").textContent,"Unknown / stale",
    "No source status at startup is unknown");
  // All Spotify write requests (including browser NEXT) must fail closed
  // when the RD45 has not reported AUX. No Spotify tokens are needed to test this.
  await assert.rejects(vm.runInContext('spotify("/me/player/next","POST")',context),
    /require RD45 AUX1\/AUX2/, "Browser cannot skip Spotify outside AUX");
  // When CAN source is AUX1 and the radio state is fresh, the UI must expose it.
  await vm.runInContext(`(() => { const oldFetch = fetch; fetch = async (url) =>
    String(url).includes("/api/spotify/status") ?
    ({ok:true,json:async()=>({ready:true,configured:true,enabled:true,online:true,
       aux_active:true,radio_source:4,last_http:204,last_action:"next",error:""})}) : oldFetch(url);
    return refreshStandalone().finally(()=>{fetch=oldFetch;}); })()`,context);
  assert.equal(element("radioSourceState").textContent,"AUX 1","CAN source is displayed");
  assert.equal(element("standaloneState").textContent,"Active on ESP32",
    "Only active with AUX source and network");

  await vm.runInContext(`(() => { const oldFetch = fetch; fetch = async (url) =>
    String(url).includes("/api/spotify/status") ?
    ({ok:true,json:async()=>({ready:false})}) : oldFetch(url);
    return refreshStandalone().finally(()=>{fetch=oldFetch;}); })()`,context);
  assert.equal(element("standaloneState").textContent,"Controller unavailable",
    "Task allocation failure must not look like missing credentials");
  assert.match(element("standaloneError").textContent,/not running/,
    "Explain failure instead of silently hiding the task problem");
  // Background controller disables the browser forwarding path to prevent duplicate commands.
  await vm.runInContext(`(() => { const oldFetch = fetch; fetch = async (url) => String(url).includes("/api/spotify/status") ?
    ({ok:true,json:async()=>({configured:true,enabled:true,online:true,last_http:204,last_action:"next",error:""})}) : oldFetch(url);
    return refreshStandalone().finally(()=>{fetch=oldFetch;}); })()`,context);
  assert.equal(element("active").disabled,true,"No double control when ESP32 is autonomous");
  assert.equal(element("active").checked,false,"Autonomous control unchecks browser forwarding");
  // A real ESP32 persists autonomous mode between page visibility changes.
  // Keep the default fake status consistent with that stored state; otherwise
  // visibility refresh silently reverts the mock to "Not installed".
  simulatedStandalone = {ready:true,configured:true,enabled:true,online:true,
    aux_active:true,radio_source:4,last_http:204,last_action:"next",error:""};

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

  // Match the upstream dashboard: after completing a fetch, schedule exactly one
  // subsequent fetch, and stop completely while the page is hidden.
  assert.equal(intervalDelays.filter(ms => ms === 5000).length, 1, "Exactly one status timer after boot");
  const visibilityCallbacks = documentListeners.get("visibilitychange") || [];
  assert.equal(visibilityCallbacks.length, 1, "Single visibility listener");
  assert.equal(mediaTimers().length, 1, "One media timer after initial polling");

  for(let i=0;i<5;i++) {
    documents.hidden = true;
    visibilityCallbacks[0]();
    await settle();
    assert.equal(mediaTimers().length, 0, "No media polling when tab is hidden");
    documents.hidden = false;
    visibilityCallbacks[0]();
    await settle();
    assert.equal(mediaTimers().length, 1, "One resumed media timer, no duplicates");
  }
  assert.equal(intervalDelays.filter(ms => ms === 5000).length, 1,
    "Foreground/background switches cannot multiply status requests");

  await fireMediaTimer();
  assert.equal(mediaTimers().length, 1, "One media timer after fetch completes");
  assert.equal(mediaTimers()[0].delay, 2000, "Autonomous mode uses 2s browser diagnostics");
  documents.hidden = true;
  visibilityCallbacks[0]();
  await settle();
  assert.equal(mediaTimers().length, 0, "No autonomous timer left in background");
  console.log("PASS: browser syntax, SHA-256 + PKCE, stalk edges, reconnect, OAuth state, no background timer, one status timer.");
}
main().catch(error => { console.error(error); process.exitCode=1; });
