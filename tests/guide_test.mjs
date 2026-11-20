// Chordially: the user guide (HELP) checked against the app. Every operation the guide describes is done the way
// the guide says, through the interface, against the real core (tests/shellpipe), and the result is compared with
// what the guide promises. Plus every control once. Screenshots to $SHOTS.
import { chromium } from "/opt/npm-tools/node_modules/playwright/index.mjs";
import { spawn } from "child_process";
import path from "path"; import fs from "fs";
const SHOTS = process.env.SHOTS || "/tmp/cmshots"; fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0, section = "";
const ok = (c, m) => { if (c) pass++; else { fail++; console.log(`FAIL [${section}]: ${m}`); } };
const pipe = spawn("/tmp/shellpipe", [], { stdio: ["pipe", "pipe", "inherit"] });
const browser = await chromium.launch({ executablePath: "/usr/bin/google-chrome", args: ["--no-sandbox"] });
const page = await browser.newPage({ viewport: { width: 1512, height: 945 } });
const errs = []; page.on("pageerror", e => errs.push(e.message)); page.on("console", m => { if (m.type() === "error") errs.push(m.text()); });
await page.exposeFunction("__cmOut", s => pipe.stdin.write(s + "\n"));
let buf = ""; pipe.stdout.on("data", d => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); page.evaluate(x => window.__cmIn && window.__cmIn(JSON.parse(x)), l).catch(() => {}); } });
const midi = (a, b, c) => pipe.stdin.write(`MIDI ${a} ${b} ${c}\n`);
await page.goto("file://" + path.resolve("app/web/index.html"));
const W = ms => page.waitForTimeout(ms);
const st = () => page.evaluate(() => S), lv = () => page.evaluate(() => LV);
const send = o => page.evaluate(x => send(x), o);
const keyOn = n => send({ t: "key", n, v: 100 }), keyOff = n => send({ t: "key", n, v: 0 });
const pat = async i => { await page.click(`#patgrid div:nth-child(${i + 1})`); await W(250); };
const txt = sel => page.textContent(sel);
await W(900);

section = "Quick start";
await pat(0);
await page.keyboard.press("Space"); await W(500); ok((await lv()).play === 1, "Space starts");
ok((await txt("#keyto")).includes("from the sequencer"), "LCD says 'from the sequencer' while playing: " + await txt("#keyto"));
await page.keyboard.press("Space"); await W(300); ok((await lv()).play === 0, "Space stops");
await page.click("#bplay"); await W(300); ok((await lv()).play === 1, "▶ starts"); await page.click("#bstop"); await W(300); ok((await lv()).play === 0, "■ stops");

section = "Piano colours";
await pat(7);   // Ambient Bloom: STYLE OFF
ok((await st()).chord.p.style === 0, "pattern 1-8 is STYLE OFF");
await page.keyboard.down("a"); await W(300);
let L = await lv();
ok(L.c.cn.length === 4, "4 chord notes (pale)");
ok(JSON.stringify([...L.c.snd].sort()) === JSON.stringify([...L.c.cn].sort()), "STYLE OFF: all four sound together (solid = pale)");
ok(await page.$eval("#piano [data-note='60']", e => e.classList.contains("hd")), "cyan bar on C4, the key held");
ok(!L.c.cn.includes(60) || true, "(the held key need not be a chord note)");
ok(/key C4 → \S+/.test(await txt("#keyto")), "LCD: key C4 → chord: " + await txt("#keyto"));
ok((await page.$$eval("#piano .ch", e => e.length)) === 4, "4 pale keys on the piano");
await page.keyboard.up("a"); await W(300);
L = await lv(); ok(L.c.snd.length === 0 && L.c.cn.length === 0, "let go: the chord stops (STYLE OFF)");
await pat(4);   // Psy Roll: phrase
await keyOn(57); let seen = new Set(), counts = new Set();
for (let i = 0; i < 25; i++) { await W(40); const l = await lv(); l.c.snd.forEach(n => seen.add(n)); counts.add(l.c.snd.length); }
L = await lv(); const allowed = new Set([...L.c.cn, ...L.c.cn.map(n => n + 12)]);
ok([...seen].every(n => allowed.has(n)), "phrase: solid notes are chord notes (or an octave up)");
ok(counts.size > 1, "phrase: notes light one by one (sounding count varies)");
await keyOff(57); await W(400);

section = "Playing chords";
await pat(0);   // Pop Bright, STYLE ARP 8; check chords by name
await keyOn(69); await W(200); ok((await lv()).c.chord === "Am", "Pop Bright: A key gives Am: " + (await lv()).c.chord);
ok((await lv()).c.cn[0] % 12 === 9, "the key is the bass note");
await keyOff(69); await keyOn(71); await W(200); ok((await lv()).c.chord === "G/B", "B key gives G/B: " + (await lv()).c.chord); await keyOff(71); await W(150);
await keyOn(57); await W(150); const lowA = (await lv()).c.cn.join(); await keyOff(57); await keyOn(81); await W(150); const hiA = (await lv()).c.cn.join(); await keyOff(81); await W(150);
ok(lowA === hiA, "the octave you play does not matter");
await keyOn(60); await W(150); const cChord = (await lv()).c.chord; await keyOn(69); await W(150);
ok((await lv()).c.chord === "Am", "newest key wins"); await keyOff(69); await W(150);
ok((await lv()).c.chord === cChord, "let go: the chord of the key still held comes back (" + cChord + ")"); await keyOff(60); await W(200);
await page.click("#bchord"); await W(200); ok((await st()).chord.chordmode === 0, "CHORD off");
await keyOn(60); await keyOn(64); await keyOn(67); await W(200);
L = await lv(); ok(L.c.chord === "C" && L.c.cn.join() === "60,64,67", "CHORD off: the held notes are the chord, named: " + L.c.chord + " " + L.c.cn);
await keyOff(60); await keyOff(64); await keyOff(67); await page.click("#bchord"); await W(200);
await pat(7); midi(0x90, 62, 100); midi(0xB0, 64, 127); await W(150); midi(0x80, 62, 0); await W(300);
ok((await lv()).c.snd.length === 4, "sustain pedal keeps the chord after letting go");
midi(0xB0, 64, 0); await W(300); ok((await lv()).c.snd.length === 0, "pedal up: the chord stops");
await page.locator("#pads .pad").nth(9).hover(); await page.mouse.down(); await W(250);
ok((await lv()).c.chord === (await st()).chord.pads[9][0], "a pad plays its chord: " + (await lv()).c.chord);
ok(await page.$eval("#pads .pad:nth-child(10)", e => e.classList.contains("on")), "the pad of the chord sounding is outlined");
await page.mouse.up(); await W(200);

section = "CHORD section";
await pat(7); await page.click("#bhold"); await W(200); ok((await st()).chord.hold === 1 && await page.$eval("#bhold", e => e.classList.contains("on")), "HOLD on (button lit)");
await page.keyboard.down("a"); await W(200); await page.keyboard.up("a"); await W(400);
ok((await lv()).c.snd.length === 4 && (await lv()).c.held.length === 0, "HOLD: the chord keeps playing after letting go");
await page.keyboard.down("h"); await W(200); await page.keyboard.up("h"); await W(300);
ok((await lv()).c.chord === (await st()).chord.pads[9][0], "HOLD: the next key replaces it (" + (await lv()).c.chord + ")");
await page.click("#bhold"); await W(300); ok((await lv()).c.snd.length === 0 && (await lv()).c.chord === "", "HOLD off releases it");
const set0 = (await st()).chord.p.set; await page.click('.sel[data-f="set"] .box button:last-child'); await W(250);
ok((await st()).chord.p.set === set0 + 1, "CHORD SET ▶ next set");
await page.click('.sel[data-f="set"] .box button:first-child'); await W(250);
await page.click('.sel[data-f="key"] .box button:last-child'); await page.click('.sel[data-f="key"] .box button:last-child'); await W(300);
const S2 = await st(); ok(S2.chord.p.key === 2, "KEY +2");
await keyOn(62); await W(200); ok((await lv()).c.chord === S2.chord.pads[0][0], "KEY D: the set's first chord is on the D key (" + (await lv()).c.chord + ")"); await keyOff(62); await W(150);
ok(S2.chord.pads[0][0].startsWith("D"), "pad 1 shows the transposed first chord: " + S2.chord.pads[0][0]);
await send({ t: "set", f: "key", v: 0 }); await W(200);
await keyOn(60); await W(150); const m1 = (await lv()).c.cn.slice(1).reduce((a, b) => a + b) / 3;
await send({ t: "set", f: "center", v: 58 }); await W(250); const m2 = (await lv()).c.cn.slice(1).reduce((a, b) => a + b) / 3;
ok(m2 < m1 - 3, `VOICING lower = upper notes lower (${m1.toFixed(1)} → ${m2.toFixed(1)}) and the held chord follows at once`);
await send({ t: "set", f: "center", v: 66 }); await keyOff(60); await W(200);

section = "PHRASE section";
const g = await page.$('.knob[data-f="gate"]'); await g.dblclick(); await W(200); ok((await st()).chord.p.gate === 80, "double-click GATE = default 80 %");
await page.hover('.knob[data-f="grnd"]'); await page.mouse.wheel(0, -100); await W(200); ok((await st()).chord.p.grnd === 1, "wheel steps GATE RND by 1");
await page.hover('.knob[data-f="swing"]'); await page.mouse.wheel(0, -100); await W(200); ok((await st()).chord.p.swing === 1, "SWING wheel");
await keyOn(60); await W(150); const c0 = (await lv()).c.cn;
await send({ t: "set", f: "oct", v: 1 }); await W(250); const c1 = (await lv()).c.cn;
ok(c1.join() === c0.map(n => n + 12).join(), "OCTAVE +1 moves everything an octave up, held chord follows");
ok((await lv()).c.snd.length === 4, "after OCTAVE the held chord still sounds (bug fixed)");
await send({ t: "set", f: "oct", v: 0 }); await keyOff(60); await W(200);
await page.click("#bhelp"); await W(300);
ok(await page.$$eval("#helpstyles tr", e => e.length) === 9, "HELP lists the 8 styles with their variations");
ok(await page.$$eval("#helpnav a", e => e.length) === 14, "HELP has 14 sections in its menu");
ok(/GPL/.test(await page.textContent("#h-about + p + p")) && /not made, endorsed/.test(await page.textContent("#helptext")), "HELP: About and licence (GPL-3.0, independent, trademarks)");
await page.screenshot({ path: SHOTS + "/10-help.png" });
await page.keyboard.press("Escape"); await W(200); ok(!(await page.$eval("#help", e => e.classList.contains("show"))), "Esc closes HELP");
await page.keyboard.press("Shift+Slash"); await W(200); ok(await page.$eval("#help", e => e.classList.contains("show")), "? opens HELP"); await page.keyboard.press("Escape");
await pat(0); const b1 = (await st()).chord.p.bpm10; await pat(4); const b5 = (await st()).chord.p.bpm10;
ok(b1 === 1100 && b5 === 1420, "tempo belongs to the pattern (110 / 142)");

section = "SOUND";
const snd0 = await txt("#i_sound"); await page.click('.sel[data-f="sound"] .box button:last-child'); await W(250);
ok((await txt("#i_sound")) !== snd0, "SOUND ▶ changes the sound (LCD)");
await page.$eval('.knob[data-f="cut"]', e => e.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }))); await W(200);
ok(/Hz/.test(await txt('.knob[data-f="cut"] .kv')) && (await st()).chord.p.eset[5] === 0, "FILTER shows the sound's own cutoff in Hz (double-click = the sound's value)");
ok((await st()).chord.p.cut >= 0 && (await st()).chord.p.cut <= 127 && (await st()).chord.p.res >= 0 && (await st()).chord.p.res <= 127, "FILTER / RESO are 0..127, never negative");
{ const sb = await page.$('.knob[data-f="sub"]'); ok(!!sb, "SUB knob next to FILTER / RESO");
  const has = (await st()).chord.p.ehas[7]; ok((await sb.evaluate(e => e.classList.contains("na"))) === !has, "SUB dimmed exactly when the sound has no sub oscillator"); }
ok(await page.$("#plugsel") && await page.$("#plugwin") && await page.$("#plugbyp"), "AU INSERT picker, EDIT, BYP present");
// ADSR: real values, loaded with every sound and shown on the knobs and in the drawing
{ let bad = 0, paths = new Set();
  const NS = (await st()).chord.sounds.length;
  for (let k = 0; k < NS; k++) {
    await send({ t: "set", f: "sound", v: k }); await W(90);
    const p = (await st()).chord.p;
    const shown = await page.$$eval('.knob[data-env] .kv', e => e.map(x => x.textContent));
    const want = p.etext.map((t, i) => p.ehas[i] ? t : "—");
    if (shown.join("|") !== want.join("|")) { bad++; if (bad < 4) console.log("   knobs", shown, "state", p.etext); }
    paths.add(await page.$eval("#envgraph path", e => e.getAttribute("d")));
  }
  ok(bad === 0, `${NS} sounds: the ATTACK … EG INT, FILTER, RESO, SUB knobs show each sound's own values`);
  ok(paths.size > 15, `the envelope drawing follows the sound (${paths.size} different shapes)`); }
await send({ t: "set", f: "sound", v: 9 }); await W(200);
ok(/(ms|s)$/.test(await txt('.knob[data-f="ea"] .kv')) && /%$/.test(await txt('.knob[data-f="es"] .kv')), "ATTACK in ms / s, SUSTAIN in %: " + await txt('.knob[data-f="ea"] .kv') + " / " + await txt('.knob[data-f="es"] .kv'));
const d0 = await page.$eval("#envgraph path", e => e.getAttribute("d"));
await page.hover('.knob[data-f="ea"]'); for (let i = 0; i < 30; i++) await page.mouse.wheel(0, -100); await W(300);
ok((await page.$eval("#envgraph path", e => e.getAttribute("d"))) !== d0, "turning ATTACK redraws the envelope");
await page.$eval('.knob[data-f="ea"]', e => e.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }))); await W(300);
ok((await st()).chord.p.eset[0] === 0 && (await page.$eval("#envgraph path", e => e.getAttribute("d"))) === d0, "double-click: back to the sound's own attack");
await page.hover('.knob[data-f="pan"]'); for (let i = 0; i < 10; i++) await page.mouse.wheel(0, 100); await W(300);
ok((await txt('.knob[data-f="pan"] .kv')) === "L16", "PAN shows L / C / R: " + await txt('.knob[data-f="pan"] .kv'));
await page.$eval('.knob[data-f="pan"]', e => e.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
await page.click('#sndtabs button[data-tab="eq"]'); await W(200);
ok(await page.isVisible('.tab-eq .knob[data-f="eql"]') && !(await page.isVisible('.tab-adj .knob[data-f="ea"]')), "EQ tab: the EQ knobs, the ADSR hidden");
const eqc0 = await page.$eval("#eqgraph path", e => e.getAttribute("d"));
await send({ t: "set", f: "eql", v: 127 }); await W(300);
ok((await page.$$eval('.knob[data-f="eql"] .kv', e => e.map(x => x.textContent))).includes("+12 dB"), "EQ LOW shows dB");
ok((await page.$eval("#eqgraph path", e => e.getAttribute("d"))) !== eqc0, "the EQ curve follows the knobs");
await send({ t: "set", f: "eql", v: 64 });
await page.screenshot({ path: SHOTS + "/14-eqtab.png" });
await page.click('#sndtabs button[data-tab="adj"]'); await W(200);
ok(await page.isVisible('.tab-adj .knob[data-f="ea"]'), "ADJUST tab: the ADSR again");
ok((await page.$eval("#instsel", e => e.value)) === "" && (await page.$eval("#instsel", e => e.options[0].textContent)) === "INTERNAL", "INSTRUMENT: INTERNAL by default");
await page.evaluate(() => __cmIn([{ t: "plugs", fx: [], inst: [{ id: "1:2:3", name: "Test – Synth" }] }]));
ok((await page.$$eval("#instsel option", e => e.length)) === 2, "installed instruments listed");
await send({ t: "set", f: "engine", v: 1 }); await W(400);
ok(await page.evaluate(() => document.body.classList.contains("ext-inst")), "a plugin instrument: AURA controls hidden");
ok(!(await page.isVisible('.knob[data-f="ea"]')) && !(await page.isVisible('.sel[data-f="sound"]')) && await page.isVisible("#plugnote"), "…SOUND / ADSR hidden, the plugin note shown, VOLUME stays");
ok(await page.isVisible('.row.plugonly .knob[data-f="vol"]'), "VOLUME stays with a plugin");
ok(await page.isVisible('.row.plugonly .knob[data-f="pan"]') && await page.isVisible('.row.plugonly .knob[data-f="eql"]') && await page.isVisible('.row.plugonly .knob[data-f="eqh"]'), "with a plugin: VOLUME, PAN and EQ LOW / MID / HIGH");
ok(!(await page.isVisible("#sndtabs")), "with a plugin: no ADJUST / EQ tabs");
await send({ t: "set", f: "engine", v: 0 }); await W(400);
ok(await page.isVisible('.knob[data-f="ea"]'), "INTERNAL again: AURA controls back");

section = "EFFECTS";
const dt0 = (await st()).g.dly_time; await page.click('.sel[data-f="dly_time"] .box button:last-child'); await W(200);
ok((await st()).g.dly_time === dt0 + 1, "DELAY TIME ▶"); ok((await txt('.sel[data-f="dly_time"] .val')) === (await st()).g.dly_names[dt0 + 1], "delay time shown as a note value");
await page.hover('.knob[data-f="rev_size"]'); await page.mouse.wheel(0, -100); await W(200); ok((await st()).g.rev_size === 71, "SIZE (shared) wheel");

section = "Patterns";
await pat(0); await send({ t: "set", f: "bpm10", v: 3000 }); await page.keyboard.press("Space"); await W(300);
await page.click("#patgrid div:nth-child(3)"); await W(300);
ok((await st()).chord.pat === 0 && (await lv()).c.next === 2, "while playing: the new pattern waits");
ok(await page.$eval("#patgrid div:nth-child(3)", e => e.classList.contains("nx")), "the waiting pattern blinks");
await W(3800); ok((await st()).chord.pat === 2, "it starts when the current pattern ends");
await page.keyboard.press("Space"); await W(300); await pat(0); await send({ t: "set", f: "bpm10", v: 1100 });
await page.fill("#patrename", "My Pop"); await page.press("#patrename", "Enter"); await W(300);
ok((await st()).chord.p.name === "My Pop", "rename the pattern");
await page.selectOption("#copyto", "50"); await W(300); ok((await st()).chord.pats[50][0].startsWith("My Pop"), "copy to 7-3");

section = "Steps";
await pat(30); await page.dblclick("#bclear"); await W(300);
ok((await st()).chord.steps.every(s => s[0] === 0), "CLEAR (double-click) empties the pattern");
await page.click("#bedit"); await page.click("#steps .st:nth-child(1)"); await W(150);
await keyOn(69); await keyOff(69); await W(200);
let S3 = await st(); ok(S3.chord.steps[0][0] === 4 && S3.chord.sel === 1, "EDIT: play a chord → written, cursor to step 2");
await page.click("#btie"); await W(200); S3 = await st(); ok(S3.chord.steps[1][0] === -1 && S3.chord.sel === 2, "TIE writes a tie and moves on");
await page.click("#brest"); await W(200); S3 = await st(); ok(S3.chord.steps[2][0] === 0 && S3.chord.sel === 3, "REST clears and moves on");
await page.click("#steps .st:nth-child(1)"); await page.selectOption("#ststyle", "6,0"); await W(250);
ok((await st()).chord.steps[0][3] === 6, "Step style on step 1 (STRUM)");
ok(await page.$eval("#steps .st:nth-child(1) .ss", e => e.textContent === "STR"), "the step shows its own style");
await page.click("#bedit"); await W(150);
await page.keyboard.down("Alt"); await page.locator("#steps .st").nth(0).hover(); await page.mouse.down(); await W(250);
ok((await lv()).c.cn.length === 4, "Alt-click (stopped) plays the step's chord while held");
await page.mouse.up(); await page.keyboard.up("Alt"); await W(300);
ok((await lv()).c.cn.length === 0, "…and stops when let go");
await send({ t: "set", f: "bpm10", v: 600 }); await page.click("#brec"); await page.keyboard.press("Space"); await W(150);
await keyOn(65); await W(100); await keyOff(65); await W(200);
S3 = await st(); ok(S3.chord.steps.some(s => s[0] === 4 && /F/.test(s[1])), "● REC: the chord played goes into the playing step");
await page.keyboard.press("Space"); await page.click("#brec"); await W(300);
await page.click("#brandom"); await W(300); ok(/^Random \d{3}$/.test((await st()).chord.p.name), "RANDOM names the pattern 'Random NNN'");
const first = (await st()).chord.steps[0][1];
await page.click("#bgen"); await send({ t: "set", f: "bpm10", v: 3000 }); await page.keyboard.press("Space"); await W(7000); await page.keyboard.press("Space"); await page.click("#bgen"); await W(300);
ok((await st()).chord.steps[0][1] === first, "GENERATIVE: the first chord stays home (" + first + ")");

section = "MASTER";
await page.click("#bcomp"); await W(250); ok((await st()).g.comp === 1 && (await txt("#bcomp")) === "ON", "COMPRESSOR on");
ok((await txt('.knob[data-f="crat"] .kv')) === "3.9:1" && (await txt('.knob[data-f="cthr"] .kv')) === "-10.1 dB", "RATIO / THRESH shown in real units: " + await txt('.knob[data-f="crat"] .kv') + ", " + await txt('.knob[data-f="cthr"] .kv'));
await send({ t: "g", f: "cthr", v: 20 }); await send({ t: "g", f: "crat", v: 127 }); await pat(11); await page.keyboard.press("Space"); await W(1800);
ok((await lv()).gr > 3, "the compressor reduces the level (COMP meter): " + (await lv()).gr + " dB");
await page.keyboard.press("Space"); await page.click("#bcomp"); await send({ t: "g", f: "cthr", v: 95 }); await send({ t: "g", f: "crat", v: 50 }); await W(300);
await page.click('.knob[data-f="dly_fb"]', { modifiers: ["Alt"] }); await W(150); midi(0xB0, 30, 10); await W(200); midi(0xB0, 30, 127); await W(300);
ok((await st()).g.dly_fb === 127, "Alt-click learn works on a shared knob (FEEDBACK)");
await send({ t: "ccdefault" }); await send({ t: "g", f: "dly_fb", v: 50 });

section = "S-1 / T-8";
ok(await page.$eval(".strip[data-k='0'] .dot", e => e.classList.contains("on")), "S-1 found: dot green");
ok((await lv()).pk[1] > 0.01, "S-1 USB audio is mixed in (meter moves)");
await page.click(".strip[data-k='0'] .mute"); await W(300); ok((await st()).ext[0].mute === 1, "MUTE");
await W(600); ok((await lv()).pk[1] < 0.01, "muted: S-1 silent"); await page.click(".strip[data-k='0'] .mute");
await page.selectOption(".strip[data-k='0'] .thin", "2"); await W(200); ok((await st()).ext[0].thru_in === 2, "Thru in channel 2");
await page.selectOption(".strip[data-k='0'] .thin", "0");
{ const x = (await st()).ext[0]; ok(x.audio === 1 && x.midi_in === 0 && x.start === 0, "defaults: AUDIO on, MIDI IN off, START off"); }
for (const f of ["audio", "midi_in", "start"]) {
  const b = `.strip[data-k='0'] .tg[data-f='${f}']`, v0 = (await st()).ext[0][f];
  await page.click(b); await W(200); const v1 = (await st()).ext[0][f];
  ok(v1 === 1 - v0 && (await page.$eval(b, e => e.classList.contains("on"))) === !!v1, `${f.toUpperCase()} toggles (and lights)`);
  await page.click(b); await W(150);
}
{ const opts = await page.$$eval(".strip[data-k='1'] .adev option", o => o.map(x => x.textContent));
  ok(opts[0] === "AUTO · T-8" && opts.some(t => /aggregate/.test(t)), "Audio menu: AUTO + the devices, aggregate marked");
  await page.selectOption(".strip[data-k='1'] .adev", "LIttle S-1 and T-8"); await W(250);
  ok(await page.isVisible(".strip[data-k='1'] .ach"), "an aggregate shows its input pairs");
  await page.selectOption(".strip[data-k='1'] .ach", "2"); await W(250);
  const x = (await st()).ext[1]; ok(x.adev === "LIttle S-1 and T-8" && x.ach === 2, "T-8 = aggregate inputs 3-4, stored");
  await page.selectOption(".strip[data-k='1'] .adev", ""); await W(200); ok((await st()).ext[1].adev === "", "back to AUTO"); }

ok(!(await page.isVisible("#instwin")), "SOUND: EDIT hidden with the internal synth");
await page.selectOption("#instsel", ""); await W(150);
ok(await page.evaluate(() => document.activeElement === document.body), "after picking an instrument the menu lets go of the keyboard (Mac keys play at once)");
{ const before = (await st()).chord.live_n; await page.keyboard.down("d"); await W(250); const lit = await page.$$eval("#piano .ch", e => e.length); await page.keyboard.up("d"); await W(150);
  ok(lit === 4, "Mac key plays right after the instrument menu (" + lit + " chord notes)"); }
{ // PLAY with a real mouse press, while the live view redraws 60 times a second: every click counts
  let okc = 0;
  for (let i = 0; i < 6; i++) { const was = !!(await lv()).play; const b = await page.$("#bplay"); const bb = await b.boundingBox();
    await page.mouse.move(bb.x + bb.width / 2, bb.y + bb.height / 2); await page.mouse.down(); await W(120); await page.mouse.up(); await W(350);
    if (!!(await lv()).play !== was) okc++; }
  ok(okc === 6, `PLAY: every press toggles play / stop (${okc}/6)`);
  if ((await lv()).play) { await page.click("#bstop"); await W(300); } }
section = "SETUP";
await page.click("#bsetup"); await W(300);
ok(await page.isVisible("#support") && /Windows version/.test(await page.textContent("#support")) && await page.isVisible("#bsponsor"), "SETUP: donation note with the Sponsor button");
await page.selectOption("#outport", "S-1"); await W(200); ok((await st()).chord.outport === "S-1", "chord notes out to S-1");
await page.selectOption("#outch", "3"); await W(200); ok((await st()).chord.outch === 3, "out channel 3");
await page.click("#blocal"); await W(200); ok((await st()).chord.local === 0, "built-in synth OFF"); await page.click("#blocal");
await page.selectOption("#outport", ""); await W(150);
await page.click("#bclkext"); await W(200); ok((await st()).g.clk_src === 1, "clock EXTERNAL"); await page.click("#bclkint"); await W(150);
await page.click("#btrin"); await W(200); ok((await st()).g.tr_in === 0, "start / stop in OFF"); await page.click("#btrin");
await page.click("#bpcin"); await W(200); ok((await st()).g.pc_in === 0, "program change in OFF"); await page.click("#bpcin");
ok(await page.$$eval("#clkports button.on", e => e.map(x => x.textContent).join()) === "S-1,T-8", "clock out on by default except IAC");
midi(0xC0, 12, 0); await W(400); ok((await st()).chord.pat === 12, "program change 12 → pattern 2-5");
midi(0xFA, 0, 0); await W(400); ok((await lv()).play === 1, "MIDI start (KeyStep PLAY) starts"); midi(0xFC, 0, 0); await W(400); ok((await lv()).play === 0, "MIDI stop stops");
await page.locator("#cclist div", { hasText: "REVERB" }).first().locator("button").first().click(); await W(200);
ok((await st()).learn === "rev", "LEARN waits for a knob"); midi(0xB0, 21, 50); await W(300);
ok((await st()).cc.some(c => c[0] === 21 && c[1] === "rev"), "turned knob CC 21 → REVERB");
midi(0xB0, 74, 127); await W(300); ok((await st()).chord.p.cut === 127, "KeyStep knob 1 (CC 74) = FILTER");
midi(0xB0, 71, 64); await W(300); ok((await st()).chord.p.style === 4, "KeyStep knob 2 (CC 71) = STYLE");
await page.click("#bccdef"); await W(300); ok(!(await st()).cc.some(c => c[0] === 21), "KEYSTEP DEFAULTS restores the map");
await page.click("#skins .skin:nth-child(2)"); await W(300);
ok((await st()).g.skin === 1 && (await page.evaluate(() => document.documentElement.dataset.skin)) === "paper", "SKIN: PAPER BLUE chosen and stored");
ok((await page.evaluate(() => getComputedStyle(document.body).backgroundColor)) !== "rgb(13, 14, 16)", "the page is light now");
await page.click("#skins .skin:nth-child(6)"); await W(300);
ok((await st()).g.skin === 5 && (await page.evaluate(() => document.documentElement.dataset.skin)) === "papyrus", "SKIN: PAPYRUS chosen and stored");
ok(!(await page.evaluate(() => [...document.querySelectorAll("#skins .skin .nm")].map(e => e.textContent))).includes("") , "all skin names shown");
ok((await page.evaluate(() => [...document.querySelectorAll("#sounds, #i_sound")].map(e => e.textContent).join(" "))).search(/AURA|TITAN|FATHOM/) < 0, "no engine names on the page");
await page.click("#skins .skin:nth-child(1)"); await W(300); ok((await st()).g.skin === 0, "back to STUDIO");
await page.screenshot({ path: SHOTS + "/11-setup.png" });
await page.click("#bsetupx");
await page.click('.knob[data-f="dly"]', { modifiers: ["Alt"] }); await W(200); ok((await st()).learn === "dly", "Alt-click a knob = MIDI learn"); await send({ t: "learn", s: "" });

section = "Sequencer: length, UNDO, SAVE, RESET, INIT";
await send({ t: "stop" }); await send({ t: "pattern", v: 0 }); await W(200); await page.click("#breset"); await W(300);
ok((await st()).chord.p.last === 16, "RESET: stock 1-1 has 16 steps");
await page.click("#steps .st:nth-child(40)"); await W(200); ok((await st()).chord.p.last === 16, "clicking step 40 doesn't make the pattern longer");
await page.click('#seqbtns .sel[data-f="last"] .val'); await W(100);
await page.keyboard.press("Meta+A"); await page.keyboard.type("24"); await page.keyboard.press("Enter"); await W(300);
ok((await st()).chord.p.last === 24, "Length typed in: 24 (" + (await st()).chord.p.last + ")");
ok((await txt('#seqbtns .sel[data-f="last"] .val')) === "24", "Length shows 24");
await page.click("#bundo"); await W(300); ok((await st()).chord.p.last === 16, "UNDO: back to 16");
await page.click("#binit"); await W(300); { const p = (await st()).chord.p; ok(p.last === 16 && p.name === "1-1", "INIT: an empty 16-step pattern"); }
await page.click("#chordname"); await page.keyboard.press("Meta+z"); await W(300); ok((await st()).chord.p.name === "Pop Drive", "⌘Z undoes INIT");
await page.click("#bsave"); await W(200); ok((await txt("#flk")) === "SAVED", "SAVE flashes SAVED");
await page.screenshot({ path: SHOTS + "/13-seqbtns.png" });

section = "Keys and shortcuts";
const t0 = (await st()).chord.p.bpm10; const tb = await page.$("#tempo").then(e => e.boundingBox());
await page.mouse.move(tb.x + 20, tb.y + 10); await page.mouse.down(); await page.mouse.move(tb.x + 20, tb.y - 10, { steps: 4 }); await page.mouse.up(); await W(300);
ok((await st()).chord.p.bpm10 > t0, "drag the tempo up");
await page.click("#chordname"); await page.keyboard.press("x"); await W(150); ok((await txt("#flv")) === "+1", "X: Mac keys an octave up"); await page.keyboard.press("z");
await page.keyboard.press("Space"); await W(300); await page.keyboard.press("Escape"); await W(300); ok((await lv()).play === 0, "Esc stops");
await page.keyboard.down("k"); await W(200); ok((await lv()).c.held.includes(72), "K = C5"); await page.keyboard.up("k"); await W(200);
const prevented = await page.evaluate(() => { const e = new KeyboardEvent("keydown", { code: "KeyH", cancelable: true, bubbles: true }); document.body.dispatchEvent(e); document.body.dispatchEvent(new KeyboardEvent("keyup", { code: "KeyH", bubbles: true })); return e.defaultPrevented; });
ok(prevented, "playing keys are marked handled (no macOS beep)");
await W(3500); { const l = await lv(); ok(l.c.held.length === 0 && l.c.snd.length === 0, "nothing left held or sounding at the end (held " + l.c.held + ", sounding " + l.c.snd + ")"); }
ok(errs.length === 0, "no page errors: " + errs.join(" | "));
await page.screenshot({ path: SHOTS + "/12-end.png" });
console.log(`${pass} passed, ${fail} failed`); console.log(fail ? "FAILED" : "ALL PASS");
await browser.close(); pipe.kill(); process.exit(fail ? 1 : 0);
