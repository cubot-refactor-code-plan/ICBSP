// ICBSP 接入帮助页面：只展示 integrate.py --json 的结果、收集选择；不含任何接入逻辑。
"use strict";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const KIND_TITLE = { can: "CAN", uart: "串口", spi: "SPI", pwm: "PWM", encoder: "编码器" };
const LEVEL = { ok: ["ok", "完成"], skip: ["muted", "跳过"], warn: ["warn-t", "注意"], error: ["err", "错误"] };

let state = { info: null, data: null, selection: null };
let timer = null;

// ---------- 与服务端交互 ----------

function opts() {
  return {
    board_dir: $("board_dir").value.trim() || undefined,
    app_dir: $("app_dir").value.trim() || undefined,
    ioc: $("ioc").value.trim() || undefined,
    no_vscode: $("no_vscode").checked,
    no_flash: $("no_flash").checked,
    no_clang: $("no_clang").checked,
  };
}

async function post(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  return r.json();
}

function banner(msg) {
  const b = $("banner");
  b.textContent = msg || "";
  b.classList.toggle("hidden", !msg);
}

async function scan() {
  const d = await post("/api/scan", { ...opts(), selection: state.selection });
  if (d.fatal) {
    banner(d.fatal + (d.stderr ? "\n" + d.stderr : ""));
    return;
  }
  banner("");
  state.data = d;
  if (d.selection) state.selection = d.selection;
  render();
  if (d.selection_source === "default" && !state.noticed) {
    state.noticed = true;
    $("report").insertAdjacentHTML("afterbegin", `<p class="muted">没有找到保存的选择（板级目录下的 icbsp_selection.json），从默认选择开始；写入时会保存。</p>`);
  }
}

function rescanSoon() {
  clearTimeout(timer);
  timer = setTimeout(scan, 400);
}

// ---------- 渲染 ----------

function render() {
  const d = state.data;
  $("project-info").innerHTML = `宿主：<code>${esc(d.host)}</code>　芯片：<b>${esc(d.mcu || "未知")}</b>（${esc(d.series || "-")}）　.ioc：<code>${esc(d.ioc || "没找到")}</code>`;
  renderChecks(d);
  renderPeripherals(d);
  renderSwitches(d);
  renderDevices(d);
  renderGenerated(d);
  renderReport(d);
  renderPlan(d);
}

function renderChecks(d) {
  const rows = (d.ioc_checks || []).map((r, i) => {
    const st = r.ok ? `<span class="ok">通过</span>` : `<span class="${r.level === "error" ? "err" : "warn-t"}">${r.level === "error" ? "不满足" : "建议"}</span>`;
    const box = !r.ok && r.auto_fix ? `<input type="checkbox" class="fix" data-id="${esc(r.id)}" checked>` : "";
    return `<tr><td>${box}</td><td>${st}</td><td>${esc(r.target)}</td><td>${esc(r.title)}</td><td class="muted">${esc(r.detail)}</td><td>${r.ok ? "" : esc(r.fix)}</td></tr>`;
  });
  const failed = (d.ioc_checks || []).filter((r) => !r.ok).length;
  $("checks").innerHTML =
    `<p>${failed ? `<span class="err">${failed} 项不满足</span>（勾选的项可以自动修改）` : `<span class="ok">全部通过</span>`}，共 ${(d.ioc_checks || []).length} 项。</p>` +
    `<table><tr><th>修改</th><th>结果</th><th>对象</th><th>检查</th><th>现状</th><th>怎么改</th></tr>${rows.join("")}</table>`;
}

function paramInput(prm, value, onchange, disabled) {
  const id = "p" + Math.random().toString(36).slice(2);
  let html;
  if (prm.type === "bool") html = `<input type="checkbox" id="${id}" ${value ? "checked" : ""}>`;
  else if (prm.type === "enum") html = `<select id="${id}">${prm.values.map((v) => `<option ${v === value ? "selected" : ""}>${esc(v)}</option>`).join("")}</select>`;
  else if (prm.type === "int_list") html = `<input id="${id}" value="${esc((value || []).join(", "))}">`;
  else html = `<input type="number" id="${id}" value="${esc(value)}" ${prm.min !== undefined ? `min="${prm.min}"` : ""} ${prm.max !== undefined ? `max="${prm.max}"` : ""} step="${prm.type === "float" ? "any" : "1"}">`;
  setTimeout(() => {
    const el = $(id);
    if (!el) return;
    el.disabled = !!disabled || !!prm.readonly;
    el.addEventListener("change", () => {
      let v;
      if (prm.type === "bool") v = el.checked;
      else if (prm.type === "enum") v = el.value;
      else if (prm.type === "int_list") v = el.value.split(/[,\s]+/).filter(Boolean).map(Number);
      else v = prm.type === "float" ? parseFloat(el.value) : parseInt(el.value, 10);
      onchange(v);
    });
  });
  return `<label title="${esc(prm.title)}">${esc(prm.title)} ${html}</label>`;
}

function renderPeripherals(d) {
  const sel = state.selection;
  const kinds = d.peripheral_kinds || {};
  const byKind = {};
  for (const c of d.candidates || []) (byKind[c.kind] ||= []).push(c);
  let html = "";
  for (const kind of Object.keys(KIND_TITLE)) {
    const list = byKind[kind];
    if (!list) continue;
    html += `<h3>${KIND_TITLE[kind]}</h3><table><tr><th>使用</th><th>外设</th><th>实例</th><th>参数</th><th>说明</th></tr>`;
    for (const c of list) {
      const p = sel.peripherals[c.ip] || { enabled: false, params: {} };
      const box = `<input type="checkbox" data-ip="${esc(c.ip)}" class="per-en" ${p.enabled ? "checked" : ""} ${c.usable ? "" : "disabled"}>`;
      const params = (kinds[kind]?.params || []).map((prm) =>
        paramInput(prm, p.params[prm.key], (v) => { p.params[prm.key] = v; rescanSoon(); }, !p.enabled)).join("");
      html += `<tr><td>${box}</td><td>${esc(c.ip)}</td><td><code>${esc(c.instance)}</code></td><td><div class="params">${params}</div></td><td class="muted">${esc(c.reason)}</td></tr>`;
    }
    html += "</table>";
  }
  $("peripherals").innerHTML = html || `<p class="muted">.ioc 里没有库能用的外设</p>`;
  for (const el of document.querySelectorAll(".per-en")) {
    el.addEventListener("change", () => {
      sel.peripherals[el.dataset.ip].enabled = el.checked;
      rescanSoon();
    });
  }
  const uarts = (d.candidates || []).filter((c) => c.kind === "uart" && sel.peripherals[c.ip]?.enabled);
  $("debug_uart").innerHTML = `<option value="">（不打印）</option>` + uarts.map((c) => `<option ${sel.debug_uart === c.ip ? "selected" : ""}>${esc(c.ip)}</option>`).join("");
  $("debug_uart").onchange = () => { sel.debug_uart = $("debug_uart").value || null; rescanSoon(); };
}

function renderSwitches(d) {
  const sel = state.selection;
  $("switches").innerHTML = Object.entries(d.switches || {}).map(([sid, sw]) =>
    `<label class="row"><input type="checkbox" class="sw" data-id="${esc(sid)}" ${sel.switches[sid] ? "checked" : ""}> <code>${esc(sid)}</code>　${esc(sw.title)}</label>`).join("");
  for (const el of document.querySelectorAll(".sw")) {
    el.addEventListener("change", () => { sel.switches[el.dataset.id] = el.checked ? 1 : 0; rescanSoon(); });
  }
}

function renderDevices(d) {
  const sel = state.selection;
  const cat = Object.fromEntries((d.devices || []).map((x) => [x.type, x]));
  $("devices").innerHTML = sel.devices.length
    ? sel.devices.map((dev, i) => {
        const spec = cat[dev.type] || { title: dev.type };
        const needs = Object.entries(dev.needs || {}).map(([k, v]) => `${esc(k)} = <code>${esc(v)}</code>`).join("，");
        const params = Object.entries(dev.params || {}).map(([k, v]) => `${esc(k)} = ${esc(JSON.stringify(v))}`).join("，");
        return `<div class="card"><b><code>${esc(dev.name)}</code></b>　${esc(spec.title)} <span class="tag">${esc(statusText(spec.status))}</span>
          <div class="muted">${needs}${params ? "；" + params : ""}</div><button data-i="${i}" class="del-dev">删除</button></div>`;
      }).join("")
    : `<p class="muted">还没有设备</p>`;
  for (const el of document.querySelectorAll(".del-dev")) {
    el.addEventListener("click", () => { sel.devices.splice(Number(el.dataset.i), 1); rescanSoon(); });
  }
  renderAddDevice(d);
}

function statusText(s) {
  return { tested: "已实测", board_use: "上板使用", compile_only: "只编译验证", planned: "计划中" }[s] || s || "";
}

function renderAddDevice(d) {
  const list = d.devices || [];
  const pick = $("add-type")?.value || (list.find((x) => x.available) || {}).type;
  const opt = list.map((x) => `<option value="${esc(x.type)}" ${x.type === pick ? "selected" : ""} ${x.available ? "" : "disabled"}>${esc(x.title)}${x.available ? "" : "（" + esc(x.reason) + "）"}</option>`).join("");
  const spec = list.find((x) => x.type === pick);
  let html = `<div class="row"><label>类型 <select id="add-type">${opt}</select></label>`;
  if (spec) html += ` <span class="tag">${esc(statusText(spec.status))}</span> <span class="muted">${esc(spec.status_note)}</span>`;
  html += `</div>`;
  const draft = { needs: {}, params: {} };
  if (spec && spec.available) {
    html += `<div class="row"><label>实例名 <input id="add-name" value="${esc(spec.type)}"></label></div><div class="params">`;
    for (const n of spec.needs) {
      draft.needs[n.key] = n.optional ? "" : n.options[0] || "";
      html += `<label>${esc(n.title)} <select data-need="${esc(n.key)}" class="add-need">${n.optional ? `<option value="">（不接）</option>` : ""}${n.options.map((o) => `<option>${esc(o)}</option>`).join("")}</select></label>`;
    }
    for (const prm of spec.params) {
      draft.params[prm.key] = prm.default;
      html += paramInput(prm, prm.default, (v) => { draft.params[prm.key] = v; });
    }
    html += `</div><div class="row"><button id="add-btn">添加</button></div>`;
  }
  $("add-device").innerHTML = html;
  $("add-type").onchange = () => renderAddDevice(d);
  for (const el of document.querySelectorAll(".add-need")) el.addEventListener("change", () => { draft.needs[el.dataset.need] = el.value; });
  const btn = $("add-btn");
  if (btn) btn.onclick = () => {
    const needs = Object.fromEntries(Object.entries(draft.needs).filter(([, v]) => v));
    state.selection.devices.push({ type: spec.type, name: $("add-name").value.trim(), needs, params: { ...draft.params } });
    rescanSoon();
  };
}

function renderGenerated(d) {
  const rows = Object.entries(d.generated || {}).map(([f, g]) => {
    const st = !g.exists ? `<span class="muted">还没有（会从模板新建）</span>`
      : !g.regions.length ? `<span class="err">没有生成区（手写的文件）</span>`
      : g.edited.length ? `<span class="warn-t">生成区 ${esc(g.edited.join("、"))} 被手改过</span>`
      : `<span class="ok">${esc(g.regions.join("、"))}</span>`;
    return `<tr><td><code>${esc(g.path)}</code></td><td>${st}</td></tr>`;
  });
  $("generated").innerHTML = `<table><tr><th>文件</th><th>生成区</th></tr>${rows.join("")}</table>`;
}

function renderReport(d) {
  $("report").innerHTML = (d.report || []).filter((r) => r.level !== "skip").map((r) => {
    const [cls, txt] = LEVEL[r.level] || ["", r.level];
    return `<div><span class="${cls}">[${txt}]</span> ${esc(r.step)}：${esc(r.msg)}</div>`;
  }).join("");
}

function diffHtml(text) {
  return text.split("\n").map((l) => {
    const cls = l.startsWith("+++") || l.startsWith("---") || l.startsWith("@@") ? "h" : l.startsWith("+") ? "a" : l.startsWith("-") ? "d" : "";
    return cls ? `<span class="${cls}">${esc(l)}</span>` : esc(l);
  }).join("\n");
}

function renderPlan(d) {
  const plan = d.plan || [];
  $("plan").innerHTML = d.ok === false
    ? `<p class="err">有错误，不能写入（见上面的报告）。</p>`
    : plan.length
      ? `<p>将修改 ${plan.length} 个文件：</p>` + plan.map((p) => `<details><summary>${p.kind === "create" ? "新建" : "修改"} <code>${esc(p.path)}</code></summary><pre class="diff">${diffHtml(p.diff)}</pre></details>`).join("")
      : `<p class="ok">已经和选择一致，无需修改。</p>`;
  $("apply").disabled = d.ok === false || !plan.length;
}

// ---------- 动作 ----------

async function fixIoc(plan) {
  const ids = [...document.querySelectorAll(".fix:checked")].map((el) => el.dataset.id);
  if (!ids.length) return alert("没有勾选要修改的项");
  if (!plan && !confirm(`修改 .ioc 里的 ${ids.length} 项（原文件会备份）${$("regenerate").checked ? "，并用 CubeMX 重新生成工程（关掉桌面上的 CubeMX）" : ""}？`)) return;
  const out = $("fix-out");
  out.classList.remove("hidden");
  out.textContent = plan ? "计算中…" : "修改中…（CubeMX 重新生成可能要几十秒）";
  const r = await post("/api/fix-ioc", { ids, plan, regenerate: !plan && $("regenerate").checked, cubemx: $("cubemx").value.trim() || undefined, ioc: opts().ioc, selection: state.selection });
  if (r.fatal) { out.textContent = r.fatal; return; }
  const lines = (r.report || []).map((x) => `[${(LEVEL[x.level] || ["", x.level])[1]}] ${x.step}：${x.msg}`).join("\n");
  out.innerHTML = esc(lines) + (r.ioc_diff ? "\n\n" + diffHtml(r.ioc_diff) : "") + (r.cubemx_output ? "\n\nCubeMX 输出：\n" + esc(r.cubemx_output) : "");
  if (!plan) scan();
}

async function apply() {
  if (!confirm("把预览里的修改写入宿主工程？")) return;
  const r = await post("/api/apply", { ...opts(), selection: state.selection, overwrite_edited: $("overwrite").checked });
  const out = $("apply-out");
  if (r.fatal) { out.innerHTML = `<p class="err">${esc(r.fatal)}</p>`; return; }
  out.innerHTML = r.applied ? `<p class="ok">已写入。下一步：cmake --preset Debug && cmake --build --preset Debug</p>` : `<p class="err">没有写入（见报告）。</p>`;
  state.data = r;
  if (r.selection) state.selection = r.selection;
  render();
  setTimeout(scan, 300);
}

async function init() {
  const info = await (await fetch("/api/info")).json();
  state.info = info;
  if (!info.python.ok) banner(info.python.error);
  else if (!info.hostOk) banner(`${info.host} 不是宿主工程根目录（没有 CMakeLists.txt）：用 --host 重新启动服务`);
  for (const id of ["board_dir", "app_dir", "ioc", "no_vscode", "no_flash", "no_clang"]) $(id).addEventListener("change", rescanSoon);
  $("rescan").onclick = scan;
  $("fix-preview").onclick = () => fixIoc(true);
  $("fix-apply").onclick = () => fixIoc(false);
  $("apply").onclick = apply;
  // 第一次不带选择：拿到存档（icbsp_selection.json）或默认选择；再带着它扫一次，预览里才有生成区的变化
  if (info.python.ok && info.hostOk) scan().then(() => state.selection && scan());
}

init();
