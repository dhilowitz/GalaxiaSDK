#!/usr/bin/env node
// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 David Hilowitz
//
// docs/installer/gx_update.js against a simulated pedal (no hardware, no browser), and its bytes against
// felucca-bsp's tools/fm1_install.py, which gx_update.py uses.
//   node tests/web_update_test.mjs PACKAGE.fwsc          (tools/gx_pkg.py package makes one)
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as G from "../docs/installer/gx_update.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
Object.assign(G.DELAY, { open: 0, start: 5, loader: 5, reboot: 5, retry: 10, poll: 20, hs: 50,
  idleCheck: 200, idleWrite: 200, waitLoader: 500, waitReboot: 500 });

let failed = 0;
function ok(cond, what) {
  console.log(what.padEnd(66) + (cond ? "ok" : "FAIL"));
  if (!cond) failed++;
}
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
async function code(p) {
  try { await p; return null; } catch (e) { return e.code || String(e); }
}

// --------------------------------------------------------- simulated pedal ---
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UP = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

function identityPkt(text) {
  const d = new Array(34).fill(0);
  d[1] = 0x59; d[2] = 0x11;
  [...text].forEach((c, i) => { d[6 + i] = c.charCodeAt(0); });
  return [0xF0, ...G.pack7(d), 0xF7];
}
function requestPkt(addr, len, flags = 0) {
  const u = [0x00, 0x59, 0x30, 8, 0, 0, flags, addr & 255, addr >>> 8 & 255, addr >>> 16 & 255, addr >>> 24 & 255,
    len & 255, len >> 8 & 255, 0];
  let sum = 0;
  for (let i = 6; i < 14; i++) sum += u[i];
  u.push(~sum & 255);
  return [0xF0, ...G.pack7(u), 0xF7];
}

class Port {
  constructor(name, onSend) { this.name = name; this.onSend = onSend; this.state = "connected"; this.onmidimessage = null; }
  async open() { if (this.state === "disconnected") throw new Error("gone"); }
  async close() {}
  send(bytes) {
    if (this.state === "disconnected") throw new Error("gone");
    if (bytes.some((b, i) => b > 127 && i && i !== bytes.length - 1)) throw new Error("bad sysex");
    this.onSend(Array.from(bytes));
  }
  deliver(pkt) { if (this.onmidimessage) this.onmidimessage({ data: Uint8Array.from(pkt) }); }
}

// a MIDIAccess holding one pedal (and optionally other gear); reads the package as the firmware and the loader do
class Pedal {
  constructor(image, identity, name, opts = {}) {
    Object.assign(this, { image, opts, upgrades: 0, readBack: new Uint8Array(image.length), reads: 0, other: [] });
    this.inputs = new Map(); this.outputs = new Map();
    this.gear = new Port("Scarlett 2i2 USB", (b) => this.other.push(b));
    this.inputs.set("gear", this.gear); this.outputs.set("gear", this.gear);
    this.boot(identity, name);
  }
  boot(identity, name) {
    if (this.port) this.port.state = "disconnected";
    this.identity = identity;
    this.port = name ? new Port(name, (b) => this.rx(b)) : null;
    this.inputs.delete("pedal"); this.outputs.delete("pedal");
    if (this.port) { this.inputs.set("pedal", this.port); this.outputs.set("pedal", this.port); }
    this.pending = null;
  }
  ask(addr, len, flags) {
    this.pending = { addr, len };
    setTimeout(() => this.port && this.port.deliver(requestPkt(addr, len, flags)), 0);
  }
  rx(b) {
    if (same(b, HS)) { setTimeout(() => this.port && this.port.deliver(identityPkt(this.identity)), 0); return; }
    if (same(b, UP)) {
      this.upgrades++;
      this.plan = this.identity.startsWith("ota-") ? this.writePlan() : this.checkPlan();
      this.next();
      return;
    }
    if (this.opts.badReply) return;
    const u = G.unpack7(b.slice(1, -1));
    if (u[0] === 0 && u[1] === 0x59 && u[2] === 0x30) this.reply(u);
  }
  checkPlan() { return [[0, 512], [4096, 512], [8192, 100], [0xE0000000, 8]]; }
  writePlan() {
    const p = [];
    for (let a = 0; a < this.image.length; a += 512) p.push([a, Math.min(512, this.image.length - a)]);
    p.push([0xF0000000, 8]);
    return p;
  }
  next() {
    const r = this.plan.shift();
    if (r) this.ask(r[0], r[1], 1);
  }
  reply(u) {
    const addr = (u[7] | u[8] << 8 | u[9] << 16 | u[10] << 24) >>> 0, n = u[11] | u[12] << 8;
    if (addr === 0xE0000000) {
      if (this.opts.noLoader) { this.boot("", null); return; }
      this.boot("ota-" + this.opts.running, "Galaxia Update");
      return;
    }
    if (addr === 0xF0000000) { this.boot(this.opts.after, this.opts.afterPort ?? "Galaxia Reverb"); return; }
    if (!this.identity.startsWith("ota-")) { this.next(); return; }
    this.readBack.set(u.slice(14, 14 + n), addr);
    this.reads++;
    if (this.opts.dropAfter && this.reads === this.opts.dropAfter) { this.boot("", null); return; }
    if (this.opts.freezeAfter && this.reads === this.opts.freezeAfter) return;
    this.next();
  }
}

// --------------------------------------------------------------------- run ---
const pkgPath = process.argv[2];
if (!pkgPath) { console.error("usage: node tests/web_update_test.mjs PACKAGE.fwsc"); process.exit(2); }
const raw = new Uint8Array(readFileSync(pkgPath));
const pkg = G.readPackage(raw);
const PRODUCT = pkg.product;

// the same bytes as the Python tool
const script = `
import base64, hashlib, json, random, sys
sys.path.insert(0, sys.argv[2])
import fm1_install as I
raw = open(sys.argv[1], "rb").read()
r = random.Random(1234)
cases = []
for n in (0, 1, 2, 6, 7, 8, 13, 255, 511, 512):
    d, a, f = bytes(r.randrange(256) for _ in range(n)), r.randrange(1 << 32), r.randrange(256)
    cases.append(dict(data=list(d), addr=a, fl=f, pack=list(I.pack7(d)), resp=list(I.response(a, d, f))))
print(json.dumps(dict(product=I.product_of(raw), sha=hashlib.sha256(I.logical_image(raw)).hexdigest(),
    hs=list(I.HS_QUERY), up=list(I.UPGRADE), cases=cases)))
`;
const py = JSON.parse(execFileSync("python3", ["-c", script, pkgPath, path.join(ROOT, "bsp", "tools")], { maxBuffer: 1 << 24 }));
ok(py.cases.every((c) => same(G.pack7(c.data), c.pack)), "pack7 matches fm1_install");
ok(py.cases.every((c) => same(G.unpack7(c.pack).slice(0, c.data.length), c.data)), "unpack7 undoes it");
ok(py.cases.every((c) => same(G.response(c.addr, Uint8Array.from(c.data), c.fl), c.resp)), "replies match fm1_install");
ok(same(HS, py.hs) && same(UP, py.up), "this test's commands match fm1_install");
ok(PRODUCT === py.product, `package identity matches (${PRODUCT})`);
const sha = (await import("node:crypto")).createHash("sha256").update(pkg.image).digest("hex");
ok(sha === py.sha, "logical image matches fm1_install");
const req = G.parseRequest(requestPkt(0x12345, 512, 3));
ok(req.addr === 0x12345 && req.len === 512 && req.flags === 3, "a read request parses");
ok(G.parseRequest(requestPkt(0, 1).map((b, i) => (i === 3 ? b ^ 1 : b))) === null, "a corrupt request is refused");
const pid = G.parseIdentity(identityPkt("ota-SDK-Galaxia_900"));
ok(pid.loader && G.modelOf(pid.text) === G.MODEL && pid.version === 900, "a loader identity parses");

// packages
const refused = (r) => { try { G.readPackage(r); return false; } catch (e) { return e.code === "badpkg"; } };
ok(refused(raw.subarray(0, 100)), "a short file is refused");
ok(refused(new Uint8Array(raw.length).fill(0x7D)), "a file with no identity is refused");
const noMark = raw.slice();
for (let i = noMark.indexOf(0x46); i >= 0; i = noMark.indexOf(0x46, i + 1)) {
  if (String.fromCharCode(...noMark.subarray(i, i + 16)) === "FELUCCA-LOADER-1") noMark[i] = 0;
}
ok(refused(noMark), "a package without the update loader is refused");
const other = raw.slice();
for (let i = 0; i < 20; i++) other[i * 0x30 + 0x2F] = (("FM-1_906".charCodeAt(i) || (0x7D - i - 1)) + i + 1) & 255;
ok(G.productOf(other) === "FM-1_906" && refused(other), "another product's package is refused");

const opts = { running: PRODUCT, after: PRODUCT };
const fresh = (o = {}) => new Pedal(pkg.image, PRODUCT, "Galaxia Delay", { ...opts, ...o });

// a full install
{
  const p = fresh(), up = new G.Updater(p), steps = [];
  const dev = await up.find();
  ok(dev && dev.id.text === PRODUCT && dev.name === "Galaxia Delay", "the pedal is found");
  const got = await up.install(dev, pkg, (k, a) => steps.push([k, a]));
  ok(got === PRODUCT, "install finishes and verifies the identity");
  ok(same(p.readBack, pkg.image), "the loader read the whole package, byte for byte");
  ok(p.upgrades === 2, "one upgrade command each to the firmware and the loader");
  const order = steps.map((s) => s[0]).filter((k, i, a) => k !== a[i - 1]).join(" ");
  ok(order === "start check loader write reboot done", `steps in order (${order})`);
  const pct = steps.filter((s) => s[0] === "write").map((s) => s[1]);
  ok(pct.at(-1) === 100 && pct.every((x, i) => !i || x >= pct[i - 1]), "write progress rises to 100");
  ok(p.other.length === 0, "other gear was never probed");
}
// resuming in the loader
{
  const p = new Pedal(pkg.image, "ota-" + PRODUCT, "Galaxia Update", opts), up = new G.Updater(p);
  const dev = await up.find();
  ok(dev.id.loader, "a pedal in update mode is recognised");
  ok(await up.install(dev, pkg) === PRODUCT && same(p.readBack, pkg.image) && p.upgrades === 1, "the install finishes the write");
}
// a stock pedal
{
  const p = new Pedal(pkg.image, "Galaxia_002", "Galaxia", opts), up = new G.Updater(p);
  const dev = await up.find();
  ok(await code(up.install(dev, pkg)) === "stock" && p.upgrades === 0, "a stock pedal is never sent the upgrade command");
}
// failures
{
  const up = new G.Updater(new Pedal(pkg.image, "", null, opts));
  ok(await up.find() === null, "no pedal: nothing found");
  ok(up.inputNames().join() === "Scarlett 2i2 USB", "the port list is there to report");
}
{
  const p = fresh({ noLoader: true }), up = new G.Updater(p);
  ok(await code(up.install(await up.find(), pkg)) === "noloader", "a loader that never appears is reported");
}
{
  const p = fresh({ dropAfter: 5 }), up = new G.Updater(p);
  ok(await code(up.install(await up.find(), pkg)) === "lost", "a pedal lost during the write is reported");
}
{
  const p = fresh({ freezeAfter: 5 }), up = new G.Updater(p);
  ok(await code(up.install(await up.find(), pkg)) === "stopped", "a loader that goes quiet is reported");
}
{
  const p = fresh({ after: "SDK-Galaxia_901" }), up = new G.Updater(p);
  ok(await code(up.install(await up.find(), pkg)) === "mismatch", "another identity after the install is reported");
}
{
  const p = fresh({ afterPort: "My Effect" }), up = new G.Updater(p);
  ok(await code(up.install(await up.find(), pkg)) === "noreturn", "a firmware on an unknown port name is not probed");
}
{
  const p = new Pedal(pkg.image, PRODUCT, "My Effect", { ...opts, afterPort: "My Effect" });
  ok(await new G.Updater(p).find() === null, "an unknown port name is not probed by default");
  const up = new G.Updater(p, "my eff");
  ok(await up.install(await up.find(), pkg) === PRODUCT, "the port field reaches it, and the restarted firmware");
  ok(p.other.length === 0, "the port field still leaves other gear alone");
}

console.log(failed ? `${failed} FAILED` : "all ok");
process.exit(failed ? 1 : 0);
