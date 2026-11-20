// Chordially: the UI layout audit. At several window sizes, measures every visible label, value, button, knob and
// selector: text clipped / overflowing, text not centred in its box, captions and controls not on one line within a
// row, controls overlapping, anything outside the window. Prints each problem; exit 1 if any.
import { chromium } from "/opt/npm-tools/node_modules/playwright/index.mjs";
import { spawn } from "child_process"; import path from "path"; import fs from "fs";
const SHOTS = process.env.SHOTS || "/tmp/cmshots"; fs.mkdirSync(SHOTS, { recursive: true });
const sizes = [[1512, 945, "paper"], [1512, 945, "papyrus"], [1512, 945, "arctic"], [1512, 945, "main"], [1470, 956, "main"], [1710, 1107, "main"], [1280, 800, "main"], [1512, 945, "setup"], [1512, 945, "help"], [1512, 945, "plugin"]];
let total = 0;
for (const [w, h, mode] of sizes) {
  const pipe = spawn("/tmp/shellpipe", [], { stdio: ["pipe", "pipe", "inherit"] });
  const b = await chromium.launch({ executablePath: "/usr/bin/google-chrome", args: ["--no-sandbox"] });
  const page = await b.newPage({ viewport: { width: w, height: h } });
  await page.exposeFunction("__cmOut", s => pipe.stdin.write(s + "\n"));
  let buf = ""; pipe.stdout.on("data", d => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); page.evaluate(x => window.__cmIn && window.__cmIn(JSON.parse(x)), l).catch(() => {}); } });
  await page.goto("file://" + path.resolve("app/web/index.html")); await page.waitForTimeout(900);
  // the longest names, to find clipping: a long sound, set, style and a held chord
  await page.evaluate(() => { send({ t: "pattern", v: 22 }); });
  await page.waitForTimeout(300);
  await page.evaluate(() => send({ t: "key", n: 61, v: 100 })); await page.waitForTimeout(300);
  if (mode === "paper") await page.evaluate(() => send({ t: "g", f: "skin", v: 1 }));
  if (mode === "arctic") await page.evaluate(() => send({ t: "g", f: "skin", v: 2 }));
  if (mode === "papyrus") await page.evaluate(() => send({ t: "g", f: "skin", v: 5 }));
  if (mode === "setup") await page.click("#bsetup");
  if (mode === "help") await page.click("#bhelp");
  if (mode === "plugin") { await page.evaluate(() => { __cmIn([{ t: "plugs", fx: [{ id: "a", name: "Valhalla DSP – Supermassive" }], inst: [{ id: "b", name: "Arturia – Analog Lab V" }] }, { t: "instrument", id: "b", name: "Arturia – Analog Lab V" }, { t: "insert", id: "a", name: "Valhalla DSP – Supermassive", byp: false }]); send({ t: "set", f: "engine", v: 1 }); }); }
  await page.waitForTimeout(400);
  const issues = await page.evaluate((mode) => {
    const root = mode === "setup" ? document.querySelector("#setupbox") : mode === "help" ? document.querySelector("#helpbox") : document;
    const out = [], vis = e => { const r = e.getBoundingClientRect(); const cs = getComputedStyle(e); return r.width > 0 && r.height > 0 && cs.visibility !== "hidden" && cs.display !== "none" && !e.closest("#setup:not(.show), #help:not(.show)"); };
    const name = e => (e.id ? "#" + e.id : e.className ? e.tagName.toLowerCase() + "." + String(e.className).split(" ")[0] : e.tagName.toLowerCase()) + (e.textContent ? ` "${e.textContent.trim().slice(0, 18)}"` : "");
    const textRect = e => { const r = document.createRange(); r.selectNodeContents(e); return r.getBoundingClientRect(); };
    // 1. clipped / overflowing text
    for (const e of root.querySelectorAll("button, .val, .pad .n, .pad .s, .pad .kb, .st, .kv, .kl, .lbl, .gl, #patname, #chordname, #lcdinfo b, .stx, .small, #tempo, #clk, #patgrid div, #pianolegend span, #helpnav a, .cclist div span, #clkports button, .skin .nm")) {
      if (!vis(e)) continue;
      if (e.scrollWidth > e.clientWidth + 1 && getComputedStyle(e).overflow !== "visible") out.push(`clipped text: ${name(e)} (${e.scrollWidth} > ${e.clientWidth})`);
    }
    // 2. centred text in its box
    for (const e of root.querySelectorAll("button, .val, .kv, .kl, #patgrid div, .pad, #tempo, #clk, .st")) {
      if (!vis(e) || !e.textContent.trim() || (e.classList.contains("st") && !e.querySelector(".sl").textContent)) continue;
      const r = e.getBoundingClientRect(), cs = getComputedStyle(e);
      const t = textRect(e.classList.contains("pad") ? e.querySelector(".n") : e.classList.contains("st") ? e.querySelector(".sl") : e);
      if (!t.width) continue;
      const bl = parseFloat(cs.borderLeftWidth) || 0, br = parseFloat(cs.borderRightWidth) || 0, bt = parseFloat(cs.borderTopWidth) || 0, bb = parseFloat(cs.borderBottomWidth) || 0;
      const cx = r.left + bl + (r.width - bl - br) / 2, cy = r.top + bt + (r.height - bt - bb) / 2;
      const tx = t.left + t.width / 2, ty = t.top + t.height / 2;
      const isPad = e.classList.contains("pad"), isSt = e.classList.contains("st");
      if (Math.abs(tx - cx) > 1.6) out.push(`text not centred horizontally: ${name(e)} (off ${(tx - cx).toFixed(1)} px)`);
      if (!isPad && Math.abs(ty - cy) > 1.6 && r.height < 40) out.push(`text not centred vertically: ${name(e)} (off ${(ty - cy).toFixed(1)} px)`);
    }
    // 3. rows: captions on one line, controls on one line
    for (const row of root.querySelectorAll("#controls .row, .strip .row, #master .row")) {
      if (!vis(row)) continue;
      const kids = [...row.children].filter(vis);
      const caps = kids.map(k => k.querySelector(":scope > .lbl, :scope > .kl")).filter(Boolean).map(c => Math.round(c.getBoundingClientRect().top));
      if (caps.length > 1 && Math.max(...caps) - Math.min(...caps) > 1) out.push(`captions not aligned in a row (${caps.join(", ")}): ${kids.map(name).join(" | ").slice(0, 120)}`);
      const ctl = kids.map(k => k.querySelector(":scope > .box, :scope > svg, :scope > button")).filter(Boolean).map(c => Math.round(c.getBoundingClientRect().top));
      if (ctl.length > 1 && Math.max(...ctl) - Math.min(...ctl) > 6) out.push(`controls not aligned in a row (${ctl.join(", ")}): ${kids.map(name).join(" | ").slice(0, 120)}`);
    }
    // 4. overlap of controls inside each panel, and anything outside the window
    const ctrls = [...root.querySelectorAll("button, select, input, .knob, .val, .pad, .st, #patgrid div, #tempo")].filter(vis);
    for (let i = 0; i < ctrls.length; i++) {
      const a = ctrls[i].getBoundingClientRect();
      if (root === document && (a.right > innerWidth + 0.5 || a.bottom > innerHeight + 0.5 || a.left < -0.5 || a.top < -0.5)) out.push(`outside the window: ${name(ctrls[i])} (${Math.round(a.right)}x${Math.round(a.bottom)})`);
      for (let j = i + 1; j < ctrls.length; j++) {
        if (ctrls[i].contains(ctrls[j]) || ctrls[j].contains(ctrls[i])) continue;
        const c = ctrls[j].getBoundingClientRect();
        const ox = Math.min(a.right, c.right) - Math.max(a.left, c.left), oy = Math.min(a.bottom, c.bottom) - Math.max(a.top, c.top);
        if (ox > 1 && oy > 1) out.push(`overlap: ${name(ctrls[i])} / ${name(ctrls[j])}`);
      }
    }
    return out;
  }, mode);
  await page.screenshot({ path: `${SHOTS}/audit-${w}x${h}-${mode}.png` });
  console.log(`== ${w}x${h} ${mode}: ${issues.length} issue(s)`); issues.slice(0, 40).forEach(x => console.log("   " + x));
  total += issues.length;
  await b.close(); pipe.kill();
}
console.log(total ? `FAILED: ${total} layout issue(s)` : "ALL PASS (layout)");
process.exit(total ? 1 : 0);
