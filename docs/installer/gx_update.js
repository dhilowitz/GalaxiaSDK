// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 David Hilowitz
//
// Install a firmware package on a Galaxia over Web MIDI: tools/gx_update.py for the browser, written from
// felucca-bsp's tools/fm1_install.py (MPL-2.0, Leo Kuroshita). In both steps the pedal asks for parts of the
// package and this answers: the running firmware checks the package and starts its update loader, then the loader
// reads the whole package and writes it. Nothing here touches the page, so tests/web_update_test.mjs can run it.

const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const FINISH_CHECK = 0xE0000000, FINISH_WRITE = 0xF0000000;
const MAXDATA = 512;
const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
const SUCCESS = [...new TextEncoder().encode("success"), 0];

export const MODEL = "SDK-Galaxia";
const LOADER_MARK = new TextEncoder().encode("FELUCCA-LOADER-1");

// M-VAVE's firmware on the board the SDK runs on: the models a Galaxia SDK package may be installed over
export const STOCK = { "Galaxia": "Galaxia", "ELEMENTAL PRO": "Elemental Pro" };
// M-VAVE's official packages (m-vave.com/download), by SHA-256: the identity the pedal reports once it runs one
// (the package's own identity marker cannot spell "Galaxia": its x encodes to the marker's "unused" value)
export const OFFICIAL = {
  "b26f4e5807d18e46bcb08924249d8490ebca135e370afcbc0dca47ca975a0f77": ["Galaxia_004", "Galaxia V04 (2026-11-09)"],
  "0642a11425d269b2663e0f3bc695d33773b4f20677bcdd62a474c1b1fd751c14": ["ELEMENTAL PRO_004", "Elemental Pro V04 (2026-11-06)"],
};

// The pedal's MIDI ports: "Galaxia <name>" (SDK firmware), "Galaxia Update" (the loader), "FM-1" (M-VAVE's firmware
// names its port after the FM-1, whose platform it shares) and "USB Composite Device" (M-VAVE's update loader). Only
// the handshake goes to them while looking; nothing else goes to a device that doesn't answer as one of the models
// above. Other gear is never probed unless the page's port field names it.
export const PORT_RE = /galaxia|fm-1|composite/i;

// milliseconds; the tests shorten them. There is no wait before a reply: a Galaxia does not need one, and a timer
// would slow the install to a crawl in a background tab.
export const DELAY = { open: 300, start: 2000, loader: 3000, reboot: 3000, retry: 1000, poll: 1000, hs: 1000,
  idleCheck: 8000, idleWrite: 180000, waitLoader: 60000, waitReboot: 40000 };

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class InstallError extends Error {
  constructor(code, message) { super(message); this.name = "InstallError"; this.code = code; }
}

// ------------------------------------------------------------ wire format ---

export function pack7(data) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}

export function unpack7(s) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}

export function response(addr, data, flags = 0) {
  const n = data.length;
  const body = [0x00, 0x59, 0x30, (n + 8) & 0xFF, ((n + 8) >> 8) & 0xFF, 0, flags,
    addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    n & 0xFF, (n >> 8) & 0xFF, 0, ...data];
  let sum = 0;
  for (let i = 6; i < body.length; i++) sum += body[i];
  body.push(~sum & 0xFF);
  return [0xF0, ...pack7(body), 0xF7];
}

export function parseRequest(pkt) {
  if (pkt.length < 2 || pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length !== 15 || u[0] !== 0x00 || u[1] !== 0x59 || u[2] !== 0x30) return null;
  let sum = 0;
  for (let i = 6; i < 14; i++) sum += u[i];
  if ((~sum & 0xFF) !== u[14]) return null;
  return { flags: u[6], addr: (u[7] | u[8] << 8 | u[9] << 16 | u[10] << 24) >>> 0, len: u[11] | u[12] << 8 | u[13] << 16 };
}

export function parseIdentity(pkt) {
  if (pkt.length < 2 || pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const d = unpack7(pkt.slice(1, -1));
  if (d.length !== 34 || d[0] !== 0x00 || d[1] !== 0x59 || d[2] !== 0x11) return null;
  const text = String.fromCharCode(...d.slice(6, 33)).replace(/\0+$/, "");
  // M-VAVE's update loader sometimes answers with a blank identity: that is a loader too
  if (!text) return { text: "ota-(unnamed)_0", model: "ota-(unnamed)", version: 0, loader: true };
  const m = /^([^_]+)_(\d+)$/.exec(text);
  return m ? { text, model: m[1], version: parseInt(m[2], 10), loader: /^ota-/i.test(m[1]) } : null;
}

export const modelOf = (text) => text.split("_")[0].replace(/^ota-/, "");

// --------------------------------------------------------------- package ---

export function productOf(raw) {
  if (raw.length < BLOCKS * BLK) throw new InstallError("badpkg", "this file is too short to be a package");
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = raw[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

// the image the pedal reads: the package without the 20 marker bytes
export function logicalImage(raw) {
  const out = new Uint8Array(raw.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(raw.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(raw.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

function holds(raw, mark) {
  outer: for (let i = raw.indexOf(mark[0]); i >= 0 && i <= raw.length - mark.length; i = raw.indexOf(mark[0], i + 1)) {
    for (let j = 1; j < mark.length; j++) if (raw[i + j] !== mark[j]) continue outer;
    return true;
  }
  return false;
}

export async function sha256hex(raw) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", raw));
  return Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
}

// a Galaxia SDK package or M-VAVE's official one -> {product (the identity the pedal reports once it runs it), image,
// official, label, sha256}; anything else is refused before the pedal is touched
export async function readPackage(raw) {
  const sha256 = await sha256hex(raw);
  const official = OFFICIAL[sha256];
  if (official) return { product: official[0], image: logicalImage(raw), official: true, label: official[1], sha256 };
  const product = productOf(raw);
  if (!/^[^_]+_\d+$/.test(product) || modelOf(product) !== MODEL || !holds(raw, LOADER_MARK)) {
    throw new InstallError("badpkg", "this is neither a Galaxia SDK package (tools/gx_pkg.py makes them) nor an official M-VAVE file this page knows");
  }
  return { product, image: logicalImage(raw), official: false, label: product, sha256 };
}

// why this package must not go on this pedal, or null
export function refusal(id, product) {
  if (id.loader) return null;            // an update didn't finish: the loader takes the package it is given
  const running = modelOf(id.text), model = modelOf(product);
  if (running === MODEL) return null;
  if (STOCK[running] && (model === MODEL || model === running)) return null;
  if (STOCK[running]) return `the pedal runs M-VAVE's ${STOCK[running]} firmware (${id.text}); this file is for the ${STOCK[model] || model}`;
  return `${id.text} isn't a Galaxia or an Elemental Pro`;
}

// ------------------------------------------------------------------ MIDI ---

class Link {
  constructor(input, output) {
    this.input = input; this.output = output; this.q = []; this.wake = null;
    input.onmidimessage = (ev) => {
      if (ev.data[0] !== 0xF0) return;
      this.q.push(Array.from(ev.data));
      if (this.wake) this.wake();
    };
  }

  get lost() { return this.input.state === "disconnected" || this.output.state === "disconnected"; }

  send(bytes) {
    try { this.output.send(bytes); return true; } catch (_) { return false; }
  }

  drain() { this.q.length = 0; }

  async read(ms) {
    if (!this.q.length) {
      let timer;
      await new Promise((resolve) => { this.wake = resolve; timer = setTimeout(resolve, ms); });
      clearTimeout(timer);
      this.wake = null;
    }
    return this.q.length ? this.q.shift() : null;
  }

  async close() {
    this.input.onmidimessage = null;
    await Promise.allSettled([this.input.close(), this.output.close()]);
  }
}

async function handshake(link, tries) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    if (!link.send(HS_QUERY)) return null;
    const end = Date.now() + DELAY.hs;
    for (let left = DELAY.hs; left > 0; left = end - Date.now()) {
      const pkt = await link.read(left);
      if (!pkt) break;
      const id = parseIdentity(pkt);
      if (id) return id;
    }
  }
  return null;
}

// --------------------------------------------------------------- updater ---

export class Updater {
  // access: a MIDIAccess. port: part of a port name, for a firmware named some other way.
  // names: parts of port names to look for as well as PORT_RE (a page made by tools/gx_install_page.py --find).
  constructor(access, port = "", names = []) {
    this.access = access;
    this.port = port.trim().toLowerCase();
    this.names = names.map((n) => n.toLowerCase());
  }

  inputNames() {
    return [...this.access.inputs.values()].filter((p) => p.state !== "disconnected").map((p) => p.name);
  }

  pairOutput(name) {
    const outs = [...this.access.outputs.values()].filter((p) => p.state !== "disconnected");
    const base = (s) => s.replace(/\s+\d+$/, "");      // Windows numbers each port
    const same = outs.filter((o) => o.name === name);
    if (same.length) return same[0];
    const near = outs.filter((o) => base(o.name) === base(name));
    return near.length === 1 ? near[0] : null;
  }

  candidates(onlyPort) {
    const mine = (p) => this.port && p.name.toLowerCase().includes(this.port);
    const named = (p) => PORT_RE.test(p.name) || this.names.some((n) => p.name.toLowerCase().includes(n));
    return [...this.access.inputs.values()].filter((p) => p.state !== "disconnected" &&
      (this.port && onlyPort ? mine(p) : mine(p) || named(p)));
  }

  // the first matching port that answers the handshake (and want(identity)) -> {link, id, name}
  async find(want = null, onlyPort = true) {
    for (const input of this.candidates(onlyPort)) {
      const output = this.pairOutput(input.name);
      if (!output) continue;
      try {
        await input.open();
        await output.open();
      } catch (_) {
        continue;
      }
      const link = new Link(input, output);
      await sleep(DELAY.open);     // CoreMIDI may drop a message sent just after opening
      const id = await handshake(link, 2);
      if (id && (!want || want(id))) return { link, id, name: input.name };
      await link.close();
    }
    return null;
  }

  async waitFor(want, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const dev = await this.find(want, false);
      if (dev) return dev;
      await sleep(DELAY.retry);
    }
    return null;
  }

  // answer read requests until the pedal asks for `finish`
  async serve(link, image, finish, idle, progress) {
    let served = 0, last = Date.now();
    for (;;) {
      const pkt = await link.read(DELAY.poll);
      if (!pkt) {
        if (link.lost) return { served, finished: false, lost: true };
        if (Date.now() - last > idle) return { served, finished: false, lost: false };
        continue;
      }
      const r = parseRequest(pkt);
      if (!r) continue;
      last = Date.now();
      if (r.addr === FINISH_CHECK || r.addr === FINISH_WRITE) {
        link.send(response(r.addr, SUCCESS));
        if (r.addr === finish) return { served, finished: true, lost: false };
        continue;
      }
      if (r.len > MAXDATA || r.addr + r.len > image.length) {
        throw new InstallError("badreq", `the pedal asked for ${r.addr.toString(16)}+${r.len}, outside the package`);
      }
      if (!link.send(response(r.addr, image.subarray(r.addr, r.addr + r.len), r.flags))) {
        return { served, finished: false, lost: true };
      }
      served++;
      progress(served, r.addr + r.len);
    }
  }

  // step 1: the running firmware checks the package and starts the loader
  async check(dev, image, step) {
    step("start", dev.id.text);
    dev.link.send(UPGRADE);
    await sleep(DELAY.start);
    const s = await this.serve(dev.link, image, FINISH_CHECK, DELAY.idleCheck, (n) => step("check", n));
    await dev.link.close();
    if (!s.finished) {
      throw new InstallError(s.lost ? "lost" : "stopped", `${s.lost ? "the pedal was disconnected" : "the pedal stopped answering"} after ${s.served} requests; nothing was written`);
    }
    step("loader");
    await sleep(DELAY.loader);
    const ota = await this.waitFor((id) => id.loader, DELAY.waitLoader);
    if (!ota) throw new InstallError("noloader", "the update loader did not appear");
    return ota;
  }

  // step 2: the loader reads and writes the whole package, then restarts
  async write(ota, image, step) {
    step("write", 0);
    ota.link.send(UPGRADE);
    await sleep(DELAY.start);
    const s = await this.serve(ota.link, image, FINISH_WRITE, DELAY.idleWrite,
      (_, end) => step("write", Math.min(99, Math.floor(end * 100 / image.length))));
    await ota.link.close();
    if (!s.finished) {
      throw new InstallError(s.lost ? "lost" : "stopped", `${s.lost ? "the loader was disconnected" : "the loader stopped answering"} after ${s.served} requests`);
    }
    step("write", 100);
  }

  async verify(product, step) {
    step("reboot");
    await sleep(DELAY.reboot);
    const back = await this.waitFor((id) => !id.loader, DELAY.waitReboot);
    if (!back) throw new InstallError("noreturn", "the pedal did not come back");
    await back.link.close();
    if (back.id.text !== product) throw new InstallError("mismatch", `written, but the pedal reports ${back.id.text} (expected ${product})`);
    step("done", back.id.text);
    return back.id.text;
  }

  // running firmware -> loader -> new firmware; a pedal already in update mode only needs the write.
  // Nothing is sent to a pedal that refusal() turns away.
  async install(dev, pkg, step = () => {}) {
    const why = refusal(dev.id, pkg.product);
    if (why) {
      await dev.link.close();
      throw new InstallError("refused", `${why}; nothing was sent`);
    }
    const ota = dev.id.loader ? dev : await this.check(dev, pkg.image, step);
    await this.write(ota, pkg.image, step);
    return this.verify(pkg.product, step);
  }
}
