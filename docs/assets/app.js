import { TIME_ZONE, OFFLINE_AFTER_MIN } from './config.js';
import * as api from './api.js';
import { renderStackedBars } from './chart.js';

// Known meter inputs, in a fixed order with fixed colors (same as the Atom screen).
// Any other channel name (e.g. "Water" on the boiler or canteen meter) uses --water.
const KNOWN = { Cold: '--cold', Warm: '--warm', Hot: '--hot' };
const ORDER = ['Cold', 'Warm', 'Hot'];
const channelVar = (name) => KNOWN[name] || '--water';
const sortChannels = (names) => [...names].sort((a, b) => {
  const ia = ORDER.indexOf(a); const ib = ORDER.indexOf(b);
  return (ia < 0 ? 99 : ia) - (ib < 0 ? 99 : ib) || a.localeCompare(b);
});

const KIND_LABEL = { washer: 'Washing machines', boiler: 'Boiler', canteen: 'Canteen', other: 'Other meters' };
const KIND_ORDER = ['washer', 'boiler', 'canteen', 'other'];
const ALL_WASHERS = 'washers';

const state = { days: 30, selection: ALL_WASHERS, devices: [], chartArgs: null };
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
const fmtDay = (key, short) => new Intl.DateTimeFormat('en-IE', short
  ? { day: 'numeric', month: 'short', timeZone: 'UTC' }
  : { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' }).format(new Date(`${key}T12:00:00Z`));
const ago = (iso) => {
  const min = Math.round((Date.now() - new Date(iso).getTime()) / 60000);
  if (min < 1) return 'just now';
  if (min < 60) return `${min} min ago`;
  if (min < 48 * 60) return `${Math.round(min / 60)} h ago`;
  return `${Math.round(min / 1440)} days ago`;
};
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const swatch = (name) => `<span class="swatch" style="background:var(${channelVar(name)})"></span>`;

function notice(html) {
  const n = $('notice');
  n.hidden = !html;
  n.innerHTML = html || '';
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------
function selectedDevices() {
  if (state.selection === ALL_WASHERS) return state.devices.filter((d) => d.kind === 'washer');
  return state.devices.filter((d) => String(d.id) === state.selection);
}
const selectionLabel = () => (state.selection === ALL_WASHERS
  ? 'all washing machines'
  : selectedDevices()[0]?.name || '');

function fillSelect() {
  const sel = $('machine');
  const groups = KIND_ORDER
    .map((k) => [k, state.devices.filter((d) => d.kind === k)])
    .filter(([, list]) => list.length);
  sel.innerHTML = `<option value="${ALL_WASHERS}">All washing machines</option>` + groups.map(([k, list]) =>
    `<optgroup label="${KIND_LABEL[k]}">${list.map((d) => `<option value="${d.id}">${esc(d.name)}</option>`).join('')}</optgroup>`).join('');
  if (![...sel.options].some((o) => o.value === state.selection)) state.selection = ALL_WASHERS;
  sel.value = state.selection;
}

const fromDay = (days) => dayKey(new Date(Date.now() - (days - 1) * 86400000));

// Every day in the period, zero-filled, summed per channel over the selected devices.
function dailySeries(rows, days, channels) {
  const map = new Map();
  for (let i = days - 1; i >= 0; i--) {
    const key = dayKey(new Date(Date.now() - i * 86400000));
    map.set(key, Object.fromEntries([['day', key], ...channels.map((c) => [c, 0])]));
  }
  for (const r of rows) {
    const t = map.get(r.day);
    if (t && r.channel in t) t[r.channel] += Number(r.used_l) || 0;
  }
  return [...map.values()];
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function renderDevices(totals, todayRows) {
  const box = $('devices');
  if (!state.devices.length) {
    box.innerHTML = '<p class="empty">No meters yet. They appear here after their first send.</p>';
    return;
  }
  const today = new Map(todayRows.map((r) => [`${r.device_id}|${r.channel}`, r.used_l]));
  const card = (d) => {
    const mine = totals.filter((t) => t.device_id === d.id);
    const channels = sortChannels(mine.map((t) => t.channel));
    const offline = !d.last_seen || Date.now() - new Date(d.last_seen) > OFFLINE_AFTER_MIN * 60000;
    const rows = channels.map((c) => `
      <span class="inlet">${swatch(c)}${esc(c)}</span>
      <span class="num">${fmtL(mine.find((t) => t.channel === c)?.litres)}</span>
      <span class="num">${fmtL(today.get(`${d.id}|${c}`) ?? 0)}</span>`).join('');
    return `
      <article class="card${String(d.id) === state.selection ? ' selected' : ''}" data-id="${d.id}" tabindex="0" role="button" aria-label="Show ${esc(d.name)}">
        <div class="card-head">
          <h3 title="${esc(d.name)}">${esc(d.name)}</h3>
          <span class="status ${offline ? 'off' : ''}">${d.last_seen ? (offline ? 'Offline · ' : '') + ago(d.last_seen) : 'never'}</span>
        </div>
        <div class="inlets">
          <span class="hdr">Meter</span><span class="hdr num">Total</span><span class="hdr num">Today</span>
          ${rows || '<span class="muted small">No readings yet</span><span></span><span></span>'}
        </div>
        <div class="card-foot">Wi-Fi ${d.rssi ?? '–'} dBm · firmware ${esc(d.fw || '–')}</div>
      </article>`;
  };
  box.innerHTML = KIND_ORDER
    .map((k) => [k, state.devices.filter((d) => d.kind === k)])
    .filter(([, list]) => list.length)
    .map(([k, list]) => `<h3 class="group">${KIND_LABEL[k]}</h3><div class="cards">${list.map(card).join('')}</div>`)
    .join('');
  box.querySelectorAll('.card').forEach((c) => {
    const pick = () => { state.selection = c.dataset.id; $('machine').value = state.selection; load(); };
    c.addEventListener('click', pick);
    c.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } });
  });
}

function renderDaily(series, channels) {
  const seriesDef = channels.map((c) => ({ key: c, label: c, cssVar: channelVar(c) }));
  state.chartArgs = { rows: series, series: seriesDef, formatValue: fmtAxis, formatDay: fmtDay };
  $('daily-title').textContent = `Daily usage · ${selectionLabel()}`;
  $('legend').innerHTML = channels.length > 1 ? channels.map((c) => `<span>${swatch(c)}${esc(c)}</span>`).join('') : '';
  renderStackedBars($('chart'), state.chartArgs);

  const sums = Object.fromEntries(channels.map((c) => [c, series.reduce((s, r) => s + r[c], 0)]));
  const all = Object.values(sums).reduce((a, b) => a + b, 0);
  $('period-total').textContent = `Last ${state.days} days: ${fmtL(all)}` +
    (channels.length > 1 ? ` (${channels.map((c) => `${c.toLowerCase()} ${fmtL(sums[c])}`).join(', ')}).` : '.');

  $('daily-table').innerHTML = `<table>
    <thead><tr><th>Day</th>${channels.map((c) => `<th class="num">${esc(c)}</th>`).join('')}${channels.length > 1 ? '<th class="num">Total</th>' : ''}</tr></thead>
    <tbody>${[...series].reverse().map((r) => `<tr><td>${fmtDay(r.day)}</td>${channels.map((c) => `<td class="num">${fmtL(r[c])}</td>`).join('')}${channels.length > 1 ? `<td class="num">${fmtL(channels.reduce((s, c) => s + r[c], 0))}</td>` : ''}</tr>`).join('')}</tbody>
  </table>`;
}

function renderRecent(rows) {
  if (!rows.length) {
    $('recent').innerHTML = '<p class="empty">No readings yet.</p>';
    return;
  }
  const names = new Map(state.devices.map((d) => [d.id, d.name]));
  const byReading = new Map();
  for (const r of rows) {
    if (!byReading.has(r.reading_id)) byReading.set(r.reading_id, { ...r, ch: [] });
    byReading.get(r.reading_id).ch.push(r);
  }
  const list = [...byReading.values()].slice(0, 50);
  const cell = (ch, field) => sortChannels(ch.map((c) => c.channel)).map((name) => {
    const c = ch.find((x) => x.channel === name);
    const v = c[field];
    return `<span class="pair">${swatch(name)}${v == null ? '–' : fmtL(v)}</span>`;
  }).join('');
  $('recent').innerHTML = `<table>
    <thead><tr><th>Time</th><th>Meter</th><th>Totals</th><th>Used since previous</th></tr></thead>
    <tbody>${list.map((r) => `<tr>
      <td>${dtFmt.format(new Date(r.created_at))}</td><td>${esc(names.get(r.device_id) || r.device_id)}</td>
      <td>${cell(r.ch, 'litres')}</td><td>${cell(r.ch, 'used_l')}</td></tr>`).join('')}</tbody>
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
    state.devices = await api.listDevices();
    fillSelect();
    const ids = selectedDevices().map((d) => d.id);
    const allIds = state.devices.map((d) => d.id);
    const [totals, todayRows, daily, recent] = await Promise.all([
      api.latestTotals(allIds),
      api.dailyUsage({ fromDay: dayKey(new Date()), ids: allIds }),
      ids.length ? api.dailyUsage({ fromDay: fromDay(state.days), ids }) : [],
      ids.length ? api.channelUsage({ ids, limit: 300 }) : [],
    ]);
    const channels = sortChannels(new Set(totals.filter((t) => ids.includes(t.device_id)).map((t) => t.channel)));
    renderDevices(totals, todayRows);
    renderDaily(dailySeries(daily, state.days, channels), channels);
    renderRecent(recent);
    notice('');
    $('updated').textContent = dtFmt.format(new Date());
  } catch (err) {
    console.error(err);
    notice(`Could not load the readings (${esc(err.message)}). Check the key in <code>config.js</code> and that the SQL setup was run.`);
  }
}

// CSV in "long" format (one row per meter input), easy to pivot in Excel.
async function exportCsv() {
  const btn = $('export');
  btn.disabled = true;
  try {
    const devices = new Map(state.devices.map((d) => [d.id, d]));
    const since = new Date(`${fromDay(state.days)}T00:00:00Z`).toISOString();
    const ids = selectedDevices().map((d) => d.id);
    const rows = [];
    for (let offset = 0; ; offset += 1000) {
      const page = await api.channelUsage({ ids, since, limit: 1000, offset });
      rows.push(...page);
      if (page.length < 1000) break;
    }
    const cell = (v) => (v == null ? '' : /[",\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v));
    const local = new Intl.DateTimeFormat('sv-SE', { timeZone: TIME_ZONE, dateStyle: 'short', timeStyle: 'medium' });
    const lines = rows.reverse().map((r) => {
      const d = devices.get(r.device_id) || {};
      return [local.format(new Date(r.created_at)), d.name, d.kind, r.channel, r.litres, r.used_l].map(cell).join(',');
    });
    const csv = ['time,meter,kind,channel,total_l,used_l', ...lines].join('\n');
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
$('machine').addEventListener('change', (e) => { state.selection = e.target.value; load(); });
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
  resizeTimer = setTimeout(() => { if (state.chartArgs) renderStackedBars($('chart'), state.chartArgs); }, 200);
});
setInterval(load, 5 * 60 * 1000);
load();
