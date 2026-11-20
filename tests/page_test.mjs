// Chordially: the page against the real core (tests/shellpipe). Screenshots to $SHOTS.
import { chromium } from "/opt/npm-tools/node_modules/playwright/index.mjs";
import { spawn } from "child_process";
import path from "path";
const SHOTS = process.env.SHOTS || "/tmp/cmshots";
import fs from "fs"; fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log("FAIL:", m); } };
const pipe = spawn("/tmp/shellpipe", [], { stdio: ["pipe", "pipe", "inherit"] });
const browser = await chromium.launch({ executablePath: "/usr/bin/google-chrome", args: ["--no-sandbox"] });
const page = await browser.newPage({ viewport: { width: 1512, height: 945 } });
const errs = []; page.on("pageerror", e => errs.push(e.message)); page.on("console", m => { if (m.type() === "error") errs.push(m.text()); });
await page.exposeFunction("__cmOut", s => pipe.stdin.write(s + "\n"));
let buf = ""; pipe.stdout.on("data", d => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); page.evaluate(x => window.__cmIn && window.__cmIn(JSON.parse(x)), l).catch(() => {}); } });
await page.goto("file://" + path.resolve("app/web/index.html"));
const W = ms => page.waitForTimeout(ms);
await W(800);
ok(await page.textContent("#patname") === "Pop Drive", "pattern 1 name shown");
ok((await page.textContent("#pads .pad .n")).length > 0, "pads labelled");
await page.screenshot({ path: SHOTS + "/01-start.png" });
// a chord from the Mac keyboard
await page.keyboard.down("d"); await W(300);
const cn = await page.textContent("#chordname");
ok(cn.length > 0 && cn !== "—", "chord name on the LCD: " + cn);
ok(await page.$$eval("#piano .ch", e => e.length) === 4, "4 chord notes lit on the piano");
await page.screenshot({ path: SHOTS + "/02-chord.png" });
await page.keyboard.up("d"); await W(200);
// play pattern 5 (Psy Roll)
await page.click("#patgrid div:nth-child(5)"); await W(200);
ok(await page.textContent("#patname") === "Psy Roll", "pattern 5 selected");
await page.keyboard.press("Space"); await W(1500);
ok(await page.$$eval("#piano .so", e => e.length) >= 0, "sounding notes");
ok((await page.textContent("#chordname")) !== "—", "sequencer chord shown");
ok(await page.$$eval("#steps .st.pl", e => e.length) === 1, "playing step highlighted");
await page.screenshot({ path: SHOTS + "/03-playing.png" });
await page.keyboard.press("Space"); await W(300);
// knob drag: FILTER
const k = await page.$('.knob[data-f="cut"]'); const bb = await k.boundingBox();
await page.mouse.move(bb.x + bb.width / 2, bb.y + 20); await page.mouse.down(); await page.mouse.move(bb.x + bb.width / 2, bb.y - 20, { steps: 5 }); await page.mouse.up(); await W(300);
ok(/Hz/.test(await page.textContent('.knob[data-f="cut"] .kv')), "filter knob moved: " + await page.textContent('.knob[data-f="cut"] .kv'));
// style selector
await page.click('.sel[data-f="style"] .box button:last-child'); await W(300);
ok((await page.textContent('.sel[data-f="style"] .val')) === "PULSE", "style → PULSE: " + await page.textContent('.sel[data-f="style"] .val'));
// step edit on an empty pattern
await page.click("#patgrid div:nth-child(30)"); await W(200);
await page.click("#bedit"); await W(150);
await page.click("#steps .st:nth-child(1)"); await W(150);
await page.click("#pads .pad:nth-child(1)"); await W(200);
await page.click("#btie"); await W(150);
await page.click("#pads .pad:nth-child(6)"); await W(300);
const s1 = await page.textContent("#steps .st:nth-child(1)");
ok(/C/.test(s1), "step 1 written: " + s1);
ok(await page.$eval("#steps .st:nth-child(2)", e => e.classList.contains("tie")), "step 2 tie");
await page.screenshot({ path: SHOTS + "/04-stepedit.png" });
await W(400);
ok(await page.$$eval("#piano .so", e => e.length) === 0, "no stuck notes after quick pad clicks");
await page.click("#bedit");
// setup
await page.click("#bsetup"); await W(300);
ok(await page.$$eval("#clkports button", e => e.length) === 3, "3 clock ports listed");
ok(await page.$$eval("#clkports button.on", e => e.length) === 2, "IAC off by default");
await page.screenshot({ path: SHOTS + "/05-setup.png" });
await page.click("#bsetupx");
ok(await page.$eval(".strip[data-k='0'] .dot", e => e.classList.contains("on")), "S-1 present");
// RANDOM, GENERATIVE, GATE RND, preventDefault (no beep)
await page.click("#patgrid div:nth-child(41)"); await W(200);
await page.click("#brandom"); await W(400);
ok(/^Random /.test(await page.textContent("#patname")), "RANDOM made a pattern: " + await page.textContent("#patname"));
await page.click("#bgen"); await W(300);
ok(await page.$eval("#bgen", e => e.classList.contains("on")), "GENERATIVE on");
ok(await page.$('.knob[data-f="grnd"]') !== null, "GATE RND knob");
ok(await page.$("#plugsel") !== null, "AU insert picker");
const prevented = await page.evaluate(() => { const e = new KeyboardEvent("keydown", { code: "KeyA", key: "a", cancelable: true, bubbles: true }); document.body.dispatchEvent(e); const r = e.defaultPrevented; document.body.dispatchEvent(new KeyboardEvent("keyup", { code: "KeyA", key: "a", bubbles: true })); return r; });
ok(prevented, "playing keys are marked handled (no macOS beep)");
await page.screenshot({ path: SHOTS + "/06-random.png" });
ok(errs.length === 0, "no page errors: " + errs.join(" | "));
console.log(`${pass} passed, ${fail} failed`); console.log(fail ? "FAILED" : "ALL PASS");
await browser.close(); pipe.kill(); process.exit(fail ? 1 : 0);
