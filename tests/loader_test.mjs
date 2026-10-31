// SPDX-License-Identifier: GPL-3.0-only
// The loader page (web/loader/index.html) in headless Chromium, against brk.c itself (build/proto_dev) behind a
// stand-in for Web MIDI: a synthetic break is dropped in, its tempo and bars found, sent, and the flash checked.
//   node tests/loader_test.mjs [SCREENSHOT.png]
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const { chromium } = require("playwright");

let fails = 0;
const check = (c, msg) => { console.log(c ? "  ok " + msg : "FAIL " + msg); if (!c) fails++; };
const ROOT = resolve(new URL("..", import.meta.url).pathname);
const dir = mkdtempSync(join(tmpdir(), "jungle-"));

// a stereo WAV: 50 ms of silence, then two bars at 170 BPM (kick, snare, hats), 44.1 kHz
function makeWav(path, bpm, bars, lead) {
  const sr = 44100, spb = sr * 60 / bpm / 4, len = Math.round(spb * 16 * bars), pre = Math.round(lead * sr);
  const n = pre + len, pcm = new Int16Array(n * 2);
  const BAR = "K.H.S..HH.KHS.H.";
  let seed = 1;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296) * 2 - 1;
  for (let st = 0; st < bars * 16; st++) {
    const c = BAR[st % 16], s0 = Math.round(st * spb);
    for (let i = 0; i < spb && s0 + i < len; i++) {
      const t = i / sr;
      let v = 0;
      if (c === "K") v = Math.sin(2 * Math.PI * (50 + 100 * Math.exp(-t * 40)) * t) * Math.exp(-t * 12);
      if (c === "S") v = rnd() * Math.exp(-t * 25) * 0.7 + Math.sin(2 * Math.PI * 190 * t) * Math.exp(-t * 30) * 0.4;
      if (c !== ".") v += rnd() * Math.exp(-t * 300) * 0.15;
      const q = Math.round(v * 20000);
      pcm[(pre + s0 + i) * 2] = q;
      pcm[(pre + s0 + i) * 2 + 1] = q;
    }
  }
  const b = Buffer.alloc(44 + pcm.byteLength);
  b.write("RIFF", 0); b.writeUInt32LE(36 + pcm.byteLength, 4); b.write("WAVEfmt ", 8);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(sr, 24);
  b.writeUInt32LE(sr * 4, 28); b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write("data", 36);
  b.writeUInt32LE(pcm.byteLength, 40); Buffer.from(pcm.buffer).copy(b, 44);
  writeFileSync(path, b);
  return { spb, pre };
}

const wav = join(dir, "test-break.wav");
const { spb, pre } = makeWav(wav, 170, 2, 0.05);

const dev = spawn(join(ROOT, "build/proto_dev"), [], { stdio: ["pipe", "pipe", "inherit"] });
const lines = createInterface({ input: dev.stdout });
let page, framesIn = 0;
const pending = [];
lines.on("line", l => {
  if (l === "dumped") { pending.shift()?.(); return; }
  page?.evaluate(hex => window.__fromDev(hex), l).catch(() => {});
});
const dump = path => new Promise(r => { pending.push(r); dev.stdin.write(`dump ${path}\n`); });

const browser = await chromium.launch();
page = await browser.newPage({ viewport: { width: 1100, height: 1500 } });
page.on("pageerror", e => { console.log("page error:", e.message); fails++; });
await page.exposeFunction("__toDev", hex => { framesIn++; dev.stdin.write(hex + "\n"); });
await page.addInitScript(() => {
  const input = { name: "JUNGLE FM-1", state: "connected", onmidimessage: null };
  const output = { name: "JUNGLE FM-1", state: "connected", send: d => window.__toDev(Array.from(d, b => b.toString(16).padStart(2, "0")).join("")) };
  const other = { name: "Some Synth", state: "connected", send: () => {} };
  window.__fromDev = hex => { const d = new Uint8Array(hex.match(/../g).map(h => parseInt(h, 16))); input.onmidimessage && input.onmidimessage({ data: d }); };
  navigator.requestMIDIAccess = async () => ({ inputs: new Map([["i", input]]), outputs: new Map([["o2", other], ["o", output]]), onstatechange: null });
});
await page.goto("file://" + join(ROOT, "web/loader/index.html"));

await page.click("#connect");
await page.waitForFunction(() => document.getElementById("pilltext").textContent === "FM-1 connected", null, { timeout: 5000 });
check(await page.textContent("#devline").then(t => t.includes("no break")), "connected; the FM-1 has no break yet");

await page.setInputFiles("#file", wav);
await page.waitForFunction(() => !document.getElementById("editor").hidden);
const st = await page.evaluate(() => window.jungle.state());
check(st.bars === 2, `2 bars found (${st.bars})`);
check(Math.abs(st.bpm - 170) < 0.3, `170 BPM found (${st.bpm})`);
check(Math.abs(st.trimA - pre) < 200, `the silence before it trimmed (${st.trimA}, the break at ${pre})`);
check(st.snare % 8 === 4, `the snare found on a backbeat (slice ${st.snare})`);
const off = st.starts.map((s, i) => Math.abs(s - (pre + i * spb)));
check(Math.max(...off) < 0.003 * 44100, `slices on the hits (worst ${Math.max(...off).toFixed(0)} samples off, slice ${off.indexOf(Math.max(...off))})`);

await page.click("#send");
await page.waitForFunction(() => /press PLAY|Not sent/.test(document.getElementById("progtext").textContent + document.getElementById("log").textContent), null, { timeout: 60000 });
const logText = await page.textContent("#log");
check(!/Not sent/.test(logText), "sent: " + logText.split("\n").filter(Boolean).slice(-2).join(" / "));
check(await page.textContent("#devline").then(t => t.includes("TEST-BREAK")), "the FM-1 reports the new break");

// the flash as the FM-1 would hold it
const fl = join(dir, "flash.bin");
await dump(fl);
const F = readFileSync(fl), LO = 0x97000, H = F.subarray(0x97000 - LO), u = o => H.readUInt32LE(o);
check(u(0) === 0x4B52424A && u(4) === 1, "header: magic and version");
const len = u(8), rate = u(12), bpm10 = u(16), bars = u(20), nsl = u(24), snare = u(28), crc = u(32);
check(rate === 44100 && bars === 2 && nsl === 32 && Math.abs(bpm10 - 1700) <= 1, `header: ${len} samples, ${rate} Hz, ${bpm10 / 10} BPM, ${bars} bars, ${nsl} slices, snare ${snare}`);
const pcm = F.subarray(0x98000 - LO, 0x98000 - LO + len * 2);
const crcOf = b => { let c = ~0 >>> 0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; } return (~c) >>> 0; };
check(crcOf(pcm) === crc, "the samples' CRC matches the header's");
check(crcOf(H.subarray(0, 316)) === u(316), "the header's own CRC is there (written by the FM-1)");

// a new snare, the same audio: only the header goes
const before = framesIn;
await page.evaluate(() => { document.getElementById("snap").checked = true; });
await page.click("#wave", { position: { x: 300, y: 90 }, modifiers: ["Shift"] });
await page.click("#send");
await page.waitForFunction(() => /slices only/.test(document.getElementById("progtext").textContent) || /press PLAY/.test(document.getElementById("progtext").textContent), null, { timeout: 20000 });
await page.waitForFunction(() => /press PLAY/.test(document.getElementById("progtext").textContent), null, { timeout: 20000 });
check(framesIn - before <= 3, `re-slicing sends the header alone (${framesIn - before} frames)`);
await dump(fl);
const snare2 = readFileSync(fl).subarray(0, 64).readUInt32LE(28);
check(snare2 !== snare, `the new snare is in the flash (${snare} -> ${snare2})`);

// a long one, as the Amen is: four bars at 136 BPM (7 s) only fits at a lower rate, picked on its own
const wav4 = join(dir, "four.wav");
makeWav(wav4, 136, 4, 0);
await page.setInputFiles("#file", wav4);
await page.waitForFunction(() => window.jungle.state().bars === 4 || true);
await page.waitForTimeout(300);
const s4 = await page.evaluate(() => window.jungle.state());
check(s4.bars === 4 && Math.abs(s4.bpm - 136) < 0.3, `four bars at 136 found (${s4.bars} at ${s4.bpm})`);
const r4 = await page.textContent("#s_rate");
check(/16 kHz/.test(r4), `the rate that fits picked: ${r4}`);
await page.click("#send");
await page.waitForFunction(() => /FOUR: 4 bars|Not sent/.test(document.getElementById("log").textContent.split("four.wav").pop()), null, { timeout: 60000 });
await dump(fl);
{
  const h = readFileSync(fl).subarray(0, 64);
  check(h.readUInt32LE(12) === 16000 && h.readUInt32LE(20) === 4 && h.readUInt32LE(24) === 64, `in the flash: ${h.readUInt32LE(8)} samples at ${h.readUInt32LE(12)} Hz, ${h.readUInt32LE(24)} slices`);
}

if (process.argv[2]) await page.screenshot({ path: process.argv[2], fullPage: true });
await browser.close();
dev.stdin.end();
console.log(fails ? `FAILED (${fails})` : "all ok");
process.exit(fails ? 1 : 0);
