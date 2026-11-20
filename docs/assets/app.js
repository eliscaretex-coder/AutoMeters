import { TIME_ZONE, OFFLINE_AFTER_MIN } from './config.js';
import * as api from './api.js';
import { renderStackedBars } from './chart.js';

// Inlets, in a fixed order with fixed colors (same colors as the Atom screen).
const INLETS = [
  { key: 'cold', label: 'Cold', cssVar: '--cold' },
  { key: 'warm', label: 'Warm', cssVar: '--warm' },
  { key: 'hot', label: 'Hot', cssVar: '--hot' },
];

const state = { days: 30, deviceId: '', devices: [], series: null };
const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
const nf0 = new Intl.NumberFormat('en-IE', { maximumFractionDigits: 0 });
const nf1 = new Intl.NumberFormat('en-IE', { maximumFractionDigits: 1 });
const fmtL = (v) => {
  if (v == null || Number.isNaN(Number(v))) return '–';
  const n = Number(v);
  return n >= 100000 ? `${nf1.format(n / 1000)} m³` : `${nf0.format(n)} L`;
};
const fmtAxis = (v) => (v >= 10000 ? `${nf1.format(v / 1000)} m³` : `${nf0.format(v)} L`);
const dtFmt = new Intl.DateTimeFormat('en-IE', { timeZone: TIME_ZONE, day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' });
const dayKey = (d) => new Intl.DateTimeFormat('en-CA', { timeZone: TIME_ZONE }).format(d); // YYYY-MM-DD
const fmtDay = (key, short) => {
  const d = new Date(`${key}T12:00:00Z`);
  return new Intl.DateTimeFormat('en-IE', short
    ? { day: 'numeric', month: 'short', timeZone: 'UTC' }
    : { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' }).format(d);
};
const ago = (iso) => {
  const min = Math.round((Date.now() - new Date(iso).getTime()) / 60000);
  if (min < 1) return 'just now';
  if (min < 60) return `${min} min ago`;
  if (min < 48 * 60) return `${Math.round(min / 60)} h ago`;
  return `${Math.round(min / 1440)} days ago`;
};
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const swatch = (i) => `<span class="swatch" style="background:var(${i.cssVar})"></span>`;

function notice(html) {
  const n = $('notice');
  n.hidden = !html;
  n.innerHTML = html || '';
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
function fromDay(days) {
  const d = new Date(Date.now() - (days - 1) * 86400000);
  return dayKey(d);
}

// Every day in the period, zero-filled, summed over the selected machines.
function dailySeries(rows, days) {
  const map = new Map();
  for (let i = days - 1; i >= 0; i--) {
    const key = dayKey(new Date(Date.now() - i * 86400000));
    map.set(key, { day: key, cold: 0, warm: 0, hot: 0 });
  }
  for (const r of rows) {
    const t = map.get(r.day);
    if (!t) continue;
    t.cold += Number(r.cold_l) || 0;
    t.warm += Number(r.warm_l) || 0;
    t.hot += Number(r.hot_l) || 0;
  }
  return [...map.values()];
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function renderDevices(devices, latest, todayRows) {
  const box = $('devices');
  if (!devices.length) {
    box.innerHTML = '<p class="empty">No machines yet. They appear here after their first send.</p>';
    return;
  }
  const today = new Map(todayRows.map((r) => [r.device_id, r]));
  box.innerHTML = devices.map((d) => {
    const r = latest.get(d.id);
    const t = today.get(d.id);
    const offline = !d.last_seen || Date.now() - new Date(d.last_seen) > OFFLINE_AFTER_MIN * 60000;
    const rows = INLETS.map((i) => `
      <span class="inlet">${swatch(i)}${i.label}</span>
      <span class="num">${fmtL(r?.[`${i.key}_l`])}</span>
      <span class="num">${t ? fmtL(t[`${i.key}_l`]) : '0 L'}</span>`).join('');
    return `
      <article class="card">
        <div class="card-head">
          <h3 title="${esc(d.machine)}">${esc(d.machine)}</h3>
          <span class="status ${offline ? 'off' : ''}">${d.last_seen ? (offline ? 'Offline · ' : '') + ago(d.last_seen) : 'never'}</span>
        </div>
        <div class="inlets">
          <span class="hdr">Inlet</span><span class="hdr num">Meter total</span><span class="hdr num">Today</span>
          ${rows}
        </div>
        <div class="card-foot">Wi-Fi ${d.rssi ?? '–'} dBm · firmware ${esc(d.fw || '–')} · ${esc(d.mac)}</div>
      </article>`;
  }).join('');
}

function renderDaily(series) {
  state.series = series;
  $('legend').innerHTML = INLETS.map((i) => `<span>${swatch(i)}${i.label}</span>`).join('');
  renderStackedBars($('chart'), { rows: series, series: INLETS, formatValue: fmtAxis, formatDay: fmtDay });

  const sum = (k) => series.reduce((s, r) => s + r[k], 0);
  const totals = Object.fromEntries(INLETS.map((i) => [i.key, sum(i.key)]));
  const all = totals.cold + totals.warm + totals.hot;
  $('period-total').textContent =
    `Last ${state.days} days: ${fmtL(all)} in total (cold ${fmtL(totals.cold)}, warm ${fmtL(totals.warm)}, hot ${fmtL(totals.hot)}).`;

  $('daily-table').innerHTML = `<table>
    <thead><tr><th>Day</th>${INLETS.map((i) => `<th class="num">${i.label}</th>`).join('')}<th class="num">Total</th></tr></thead>
    <tbody>${[...series].reverse().map((r) => `<tr><td>${fmtDay(r.day)}</td>${INLETS.map((i) => `<td class="num">${fmtL(r[i.key])}</td>`).join('')}<td class="num">${fmtL(r.cold + r.warm + r.hot)}</td></tr>`).join('')}</tbody>
  </table>`;
}

function renderRecent(rows) {
  if (!rows.length) {
    $('recent').innerHTML = '<p class="empty">No readings in this period.</p>';
    return;
  }
  $('recent').innerHTML = `<table>
    <thead><tr><th>Time</th><th>Machine</th>
      ${INLETS.map((i) => `<th class="num">${i.label} total</th>`).join('')}
      ${INLETS.map((i) => `<th class="num">${i.label} used</th>`).join('')}
      <th class="num">Wi-Fi</th></tr></thead>
    <tbody>${rows.map((r) => `<tr>
      <td>${dtFmt.format(new Date(r.created_at))}</td><td>${esc(r.machine)}</td>
      ${INLETS.map((i) => `<td class="num">${fmtL(r[`${i.key}_l`])}</td>`).join('')}
      ${INLETS.map((i) => `<td class="num">${r[`${i.key}_used_l`] == null ? '–' : fmtL(r[`${i.key}_used_l`])}</td>`).join('')}
      <td class="num">${r.rssi ?? '–'}</td></tr>`).join('')}</tbody>
  </table>`;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
async function load() {
  if (!api.isConfigured()) {
    notice('This page is not connected yet. Put the Supabase publishable key in <code>docs/assets/config.js</code>.');
    return;
  }
  try {
    const devices = await api.listDevices();
    state.devices = devices;
    const sel = $('machine');
    const keep = sel.value;
    sel.innerHTML = '<option value="">All machines</option>' +
      devices.map((d) => `<option value="${d.id}">${esc(d.machine)}</option>`).join('');
    sel.value = devices.some((d) => String(d.id) === keep) ? keep : '';
    state.deviceId = sel.value;

    const shown = state.deviceId ? devices.filter((d) => String(d.id) === state.deviceId) : devices;
    const [latestList, daily, recent] = await Promise.all([
      Promise.all(shown.map((d) => api.latestReading(d.id).then((r) => [d.id, r]))),
      api.dailyUsage({ fromDay: fromDay(state.days), deviceId: state.deviceId }),
      api.recentReadings({ deviceId: state.deviceId, limit: 50 }),
    ]);
    const today = dayKey(new Date());
    renderDevices(shown, new Map(latestList), daily.filter((r) => r.day === today));
    renderDaily(dailySeries(daily, state.days));
    renderRecent(recent);
    notice('');
    $('updated').textContent = dtFmt.format(new Date());
  } catch (err) {
    console.error(err);
    notice(`Could not load the readings (${esc(err.message)}). Check the key in <code>config.js</code> and that the SQL setup was run.`);
  }
}

async function exportCsv() {
  const btn = $('export');
  btn.disabled = true;
  try {
    const since = new Date(`${fromDay(state.days)}T00:00:00Z`).toISOString();
    const rows = [];
    for (let offset = 0; ; offset += 1000) {
      const page = await api.recentReadings({ deviceId: state.deviceId, since, limit: 1000, offset });
      rows.push(...page);
      if (page.length < 1000) break;
    }
    const cols = ['created_at', 'machine', 'cold_l', 'warm_l', 'hot_l', 'cold_used_l', 'warm_used_l', 'hot_used_l', 'rssi', 'fw'];
    const cell = (v) => (v == null ? '' : /[",\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v));
    const csv = [cols.join(','), ...rows.reverse().map((r) => cols.map((c) => cell(r[c])).join(','))].join('\n');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
    a.download = `water-readings-${dayKey(new Date())}.csv`;
    a.click();
    URL.revokeObjectURL(a.href);
  } catch (err) {
    notice(`Download failed (${esc(err.message)}).`);
  } finally {
    btn.disabled = false;
  }
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
$('machine').addEventListener('change', load);
$('refresh').addEventListener('click', load);
$('export').addEventListener('click', exportCsv);
document.querySelectorAll('.seg button').forEach((b) => b.addEventListener('click', () => {
  document.querySelectorAll('.seg button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
  state.days = Number(b.dataset.days);
  load();
}));
$('toggle-table').addEventListener('click', (e) => {
  const showTable = $('daily-table').hidden;
  $('daily-table').hidden = !showTable;
  $('chart').hidden = showTable;
  e.target.textContent = showTable ? 'Show as chart' : 'Show as table';
});
let resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (state.series) renderDaily(state.series); }, 200);
});
setInterval(load, 5 * 60 * 1000);
load();
