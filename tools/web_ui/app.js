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
  renderFlash(d);
  renderOptim(d);
  renderHostFiles(d);
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

// ---------- 外置 Flash / 编译优化 / 工程文件（选择里的 external_flash / optimization / gitignore） ----------

function renderFlash(d) {
  const sel = state.selection;
  const a = d.external_flash;
  const meta = d.options_meta || {};
  const layouts = (meta.options || {}).flash_layouts || {};
  const chips = meta.flash_chips || [];
  if (!sel || !a || !sel.external_flash) {
    $("flash").innerHTML = `<p class="muted">（integrate.py 没有返回外置 Flash 信息）</p>`;
    $("suggest").classList.add("hidden");
    return;
  }
  const ef = sel.external_flash;

  // 建议横幅：.ioc 有 QUADSPI，且片内 Flash 小或最近一次构建占用高
  const sug = $("suggest");
  if (a.suggest && !ef.enabled) {
    sug.innerHTML = `<b>建议启用外置 Flash</b>：${a.reasons.map(esc).join("；")}<br>
      <button class="primary" data-layout="QSPI_RAM">启用方案 A（推荐）</button><button data-layout="QSPI_XIP">启用方案 B（XIP）</button>
      <span class="muted">（启用后在“外置 Flash”一节选芯片，再预览、写入）</span>`;
    sug.classList.remove("hidden");
    for (const b of sug.querySelectorAll("button")) {
      b.onclick = () => {
        ef.enabled = true;
        ef.layout = b.dataset.layout;
        if (!ef.chip && chips.length) ef.chip = (chips.find((c) => c.status === "tested") || chips[0]).name;
        rescanSoon();
        $("flash-sec").scrollIntoView({ behavior: "smooth" });
      };
    }
  } else {
    sug.classList.add("hidden");
  }

  if (!a.supported) {
    $("flash").innerHTML = `<p class="muted">${esc(a.note || ".ioc 没有启用 QUADSPI：外置 Flash 需要先在 CubeMX 里打开 QUADSPI（单 Flash，CLK + NCS + IO0~IO3）")}</p>` +
      (ef.enabled ? `<p class="err">选择里启用了外置 Flash，但当前 .ioc 不支持 <button id="ef-off">关闭</button></p>` : "");
    const off = $("ef-off");
    if (off) off.onclick = () => { ef.enabled = false; rescanSoon(); };
    return;
  }

  const mhz = (p) => Math.floor(240 / (Number(p) + 1));
  const chip = chips.find((c) => c.name === ef.chip);
  const pins = (a.pins || []).map((p) => `${esc(p.pin)}=${esc(p.bank ? "BK" + p.bank + "_" : "")}${esc(p.signal)}`).join("　");
  const usage = a.usage ? `最近一次构建占片内 Flash ${(a.usage.ratio * 100).toFixed(0)}%（${Math.round(a.usage.bytes / 1024)} KB）` : "没找到最近的构建";
  let html = `<div class="muted">${esc(a.peripheral)} Bank ${esc(a.bank ?? "?")}：${pins}<br>片内 Flash ${esc(a.internal_kb ?? "?")} KB；${esc(usage)}</div>
    <label class="row"><input type="checkbox" id="ef-enabled" ${ef.enabled ? "checked" : ""}> 启用外置 Flash（生成 bootloader、链接脚本、预设与烧录配置）</label>`;
  if (ef.enabled) {
    html += `<div class="layouts"><b>默认程序存放方式</b>` + Object.entries(layouts).map(([k, t]) =>
      `<label><input type="radio" name="ef-layout" value="${esc(k)}" ${ef.layout === k ? "checked" : ""}> <code>${esc(k)}</code>　${esc(t)}</label>`).join("") + `</div>
      <div class="opt-grid">
        <span>芯片</span><select id="ef-chip"><option value="">（选择芯片）</option>${chips.map((c) =>
          `<option value="${esc(c.name)}" ${c.name === ef.chip ? "selected" : ""}>${esc(c.name)}（${c.size >> 20} MB，${esc(statusText(c.status))}）</option>`).join("")}</select>
        <span>QSPI 预分频</span><span><input type="number" id="ef-pre" min="0" max="255" value="${esc(ef.prescaler)}" style="width:5em">
          → 应用运行时 ${mhz(ef.prescaler)} MHz${chip ? `（${esc(chip.name)} 上限 ${chip.max_mhz} MHz）` : ""}</span>
      </div>`;
    if (a.chip_guess && a.chip_guess !== ef.chip) html += `<p class="muted">按 .ioc 的 QUADSPI Flash Size 推测是 ${esc(a.chip_guess)}</p>`;
  }
  $("flash").innerHTML = html;
  $("ef-enabled").onchange = (e) => {
    ef.enabled = e.target.checked;
    if (ef.enabled && !ef.chip && chips.length) ef.chip = (chips.find((c) => c.status === "tested") || chips[0]).name;
    rescanSoon();
  };
  for (const r of document.querySelectorAll('input[name="ef-layout"]')) r.onchange = () => { ef.layout = r.value; rescanSoon(); };
  if ($("ef-chip")) $("ef-chip").onchange = (e) => { ef.chip = e.target.value || null; rescanSoon(); };
  if ($("ef-pre")) $("ef-pre").onchange = (e) => { ef.prescaler = Number(e.target.value); rescanSoon(); };
}

function renderOptim(d) {
  const sel = state.selection;
  const opt = (d.options_meta || {}).options || {};
  if (!sel || !sel.optimization) { $("optim").innerHTML = ""; return; }
  const op = sel.optimization;
  let html = `<label class="row"><input type="checkbox" id="op-enabled" ${op.enabled ? "checked" : ""}> 由 ICBSP 管理优化等级</label>`;
  if (op.enabled) {
    html += `<div class="opt-grid">` + (opt.build_types || []).map((bt) =>
      `<span>${esc(bt)}</span><select class="op-level" data-bt="${esc(bt)}">${(opt.opt_levels || []).map((lv) =>
        `<option ${op[bt] === lv ? "selected" : ""}>${esc(lv)}</option>`).join("")}</select>`).join("") + `</div>
      <label class="row"><input type="checkbox" id="op-vendor" ${op.vendor_os ? "checked" : ""}> CubeMX 生成的源码（HAL / FreeRTOS / USB）固定 -Os：Debug 时自己的代码仍按上面的等级，可省二三十 KB</label>`;
  }
  $("optim").innerHTML = html;
  $("op-enabled").onchange = (e) => { op.enabled = e.target.checked; rescanSoon(); };
  for (const s of document.querySelectorAll(".op-level")) s.onchange = () => { op[s.dataset.bt] = s.value; rescanSoon(); };
  if ($("op-vendor")) $("op-vendor").onchange = (e) => { op.vendor_os = e.target.checked; rescanSoon(); };
}

function renderHostFiles(d) {
  const sel = state.selection;
  if (!sel) return;
  const exists = d.gitignore_exists;
  const flashOn = sel.flash_scripts !== false;
  const extOn = !!(sel.external_flash && sel.external_flash.enabled);
  const noVscode = $("no_vscode").checked;
  const buttons = ["[⚡ DAP烧录]", "[🔌 JLink烧录]"].concat(extOn ? ["[🧠 RAM运行]", "[🥾 重烧boot]"] : []);
  $("hostfiles").innerHTML = `
    <label class="row"><input type="checkbox" id="flash_scripts" ${flashOn ? "checked" : ""}>
      生成 <code>Flash/</code> 烧录脚本（OpenOCD / J-Link，按构建目录的存放方式烧）</label>
    <p class="muted" style="margin:2px 0 8px 26px">${flashOn
      ? (noVscode
        ? "上面勾了“不处理 .vscode”，所以不会加烧录任务与按钮。"
        : `同时在 <code>.vscode/tasks.json</code> 加烧录任务，在 <code>settings.json</code> 的 <code>VsCodeTaskButtons.tasks</code>（Task Buttons 插件）加状态栏按钮：${buttons.map(esc).join(" ")}`)
      : "不生成 Flash/，也不加烧录任务与按钮；宿主已有的不会删除。"}</p>
    <label class="row"><input type="checkbox" id="gitignore" ${sel.gitignore !== false ? "checked" : ""}>
      复制示例 .gitignore（<code>templates/gitignore</code>）：${exists ? "宿主已有 .gitignore，只把缺的条目追加到末尾" : "宿主还没有 .gitignore，整份复制"}</label>`;
  $("flash_scripts").onchange = (e) => { sel.flash_scripts = e.target.checked; rescanSoon(); };
  $("gitignore").onchange = (e) => { sel.gitignore = e.target.checked; rescanSoon(); };
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
  for (const id of ["board_dir", "app_dir", "ioc", "no_vscode", "no_clang"]) $(id).addEventListener("change", rescanSoon);
  $("rescan").onclick = scan;
  $("fix-preview").onclick = () => fixIoc(true);
  $("fix-apply").onclick = () => fixIoc(false);
  $("apply").onclick = apply;
  // 第一次不带选择：拿到存档（icbsp_selection.json）或默认选择；再带着它扫一次，预览里才有生成区的变化
  if (info.python.ok && info.hostOk) scan().then(() => state.selection && scan());
}

init();
