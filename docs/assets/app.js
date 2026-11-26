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

// Weeks run Monday to Sunday (Irish time). Period buttons pick whole weeks.
const PERIODS = {
  week: { label: 'This week', weeksBack: 0, last: false },
  last: { label: 'Last week', weeksBack: 1, last: true },
  w4: { label: 'Last 4 weeks', weeksBack: 3, last: false },
  w12: { label: 'Last 12 weeks', weeksBack: 11, last: false },
};
const WEEKS_SHOWN = 12;  // rows in the weekly table (also the oldest day loaded)

const state = { period: 'week', selection: ALL_WASHERS, devices: [], chartArgs: null, day: null, dayArgs: null, channels: [] };
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
const nfInt = new Intl.NumberFormat('en-IE', { maximumFractionDigits: 0, useGrouping: false });
const fmtCount = (v) => (v == null || Number.isNaN(Number(v)) ? '–' : nfInt.format(Number(v)));
const dayKey = (d) => new Intl.DateTimeFormat('en-CA', { timeZone: TIME_ZONE }).format(d); // YYYY-MM-DD
const keyDate = (key) => new Date(`${key}T12:00:00Z`);
const fmtDay = (key, short) => new Intl.DateTimeFormat('en-IE', short
  ? { day: 'numeric', month: 'short', timeZone: 'UTC' }
  : { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' }).format(keyDate(key));
const fmtWeekday = (key) => new Intl.DateTimeFormat('en-IE', { weekday: 'short', day: 'numeric', timeZone: 'UTC' }).format(keyDate(key));
const fmtShort = (key) => new Intl.DateTimeFormat('en-IE', { weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC' }).format(keyDate(key));
const fmtRange = (from, to) => `${fmtShort(from)} – ${fmtShort(to)}`;

// Calendar days as 'YYYY-MM-DD' keys (Irish dates), so no time zone maths is needed.
const addDays = (key, n) => {
  const d = keyDate(key);
  d.setUTCDate(d.getUTCDate() + n);
  return d.toISOString().slice(0, 10);
};
const mondayOf = (key) => addDays(key, -((keyDate(key).getUTCDay() + 6) % 7));
const todayKey = () => dayKey(new Date());
const thisMonday = () => mondayOf(todayKey());

// First and last day of the selected period (whole Monday-to-Sunday weeks).
function periodRange(period) {
  const p = PERIODS[period];
  const mon = thisMonday();
  const from = addDays(mon, -7 * p.weeksBack);
  const to = p.last ? addDays(mon, -1) : addDays(mon, 6);
  return { from, to };
}
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

// Every day from..to, zero-filled, summed per channel over the given rows.
function dailySeries(rows, from, to, channels) {
  const map = new Map();
  for (let key = from; key <= to; key = addDays(key, 1)) {
    map.set(key, Object.fromEntries([['day', key], ...channels.map((c) => [c, 0])]));
  }
  for (const r of rows) {
    const t = map.get(r.day);
    if (t && r.channel in t) t[r.channel] += Number(r.used_l) || 0;
  }
  return [...map.values()];
}

// Litres per channel used from..to (inclusive), over the given rows.
function usageBetween(rows, from, to, channels) {
  const out = Object.fromEntries(channels.map((c) => [c, 0]));
  for (const r of rows) {
    if (r.day >= from && r.day <= to && r.channel in out) out[r.channel] += Number(r.used_l) || 0;
  }
  return out;
}
const sumOf = (obj) => Object.values(obj).reduce((a, b) => a + b, 0);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function renderDevices(totals, weekRows) {
  const box = $('devices');
  if (!state.devices.length) {
    box.innerHTML = '<p class="empty">No meters yet. They appear here after their first send.</p>';
    return;
  }
  const week = new Map();
  for (const r of weekRows) {
    const k = `${r.device_id}|${r.channel}`;
    week.set(k, (week.get(k) || 0) + (Number(r.used_l) || 0));
  }
  const card = (d) => {
    const mine = totals.filter((t) => t.device_id === d.id);
    const channels = sortChannels(mine.map((t) => t.channel));
    const offline = !d.last_seen || Date.now() - new Date(d.last_seen) > OFFLINE_AFTER_MIN * 60000;
    const rows = channels.map((c) => {
      const t = mine.find((x) => x.channel === c);
      return `
      <span class="inlet">${swatch(c)}${esc(c)}</span>
      <span class="num">${fmtL(week.get(`${d.id}|${c}`) ?? 0)}</span>
      <span class="num">${fmtL(t?.litres)}</span>
      <span class="num counter" title="Pulses counted: should match the 7111 counter">${fmtCount(t?.pulses)}</span>`;
    }).join('');
    return `
      <article class="card${String(d.id) === state.selection ? ' selected' : ''}" data-id="${d.id}" tabindex="0" role="button" aria-label="Show ${esc(d.name)}">
        <div class="card-head">
          <h3 title="${esc(d.name)}">${esc(d.name)}</h3>
          <span class="status ${offline ? 'off' : ''}">${d.last_seen ? (offline ? 'Offline · ' : '') + ago(d.last_seen) : 'never'}</span>
        </div>
        <div class="inlets">
          <span class="hdr">Meter</span><span class="hdr num" title="Monday to Sunday">This week</span><span class="hdr num" title="Accumulated since installed">Total</span><span class="hdr num" title="Pulses counted: compare with the 7111">Counter</span>
          ${rows || '<span class="muted small">No readings yet</span><span></span><span></span><span></span>'}
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

// Tiles for the selection: today, this week, last week, accumulated total (and counters for one meter).
function renderSummary(rows, totals, ids, channels) {
  const today = todayKey();
  const mon = thisMonday();
  const breakdown = (obj, fmt = fmtL) => (channels.length > 1 || fmt !== fmtL
    ? `<div class="tile-rows">${channels.map((c) => `<span class="pair">${swatch(c)}${fmt(obj[c])}</span>`).join('')}</div>`
    : '');
  const tile = (label, sub, obj) => `<div class="tile">
      <div class="tile-label">${label}</div><div class="tile-sub">${sub}</div>
      <div class="tile-value">${fmtL(sumOf(obj))}</div>${breakdown(obj)}</div>`;

  const mine = totals.filter((t) => ids.includes(t.device_id));
  const acc = Object.fromEntries(channels.map((c) => [c, mine.filter((t) => t.channel === c).reduce((s, t) => s + (Number(t.litres) || 0), 0)]));
  let html = tile('Today', fmtShort(today), usageBetween(rows, today, today, channels)) +
    tile('This week', fmtRange(mon, addDays(mon, 6)), usageBetween(rows, mon, today, channels)) +
    tile('Last week', fmtRange(addDays(mon, -7), addDays(mon, -1)), usageBetween(rows, addDays(mon, -7), addDays(mon, -1), channels)) +
    tile('Accumulated', 'total since installed', acc);
  if (ids.length === 1) {
    const counters = Object.fromEntries(channels.map((c) => [c, mine.find((t) => t.channel === c)?.pulses]));
    html += `<div class="tile">
      <div class="tile-label">Counter</div><div class="tile-sub">compare with the 7111</div>
      <div class="tile-rows big">${channels.map((c) => `<span class="pair">${swatch(c)}<span class="counter">${fmtCount(counters[c])}</span></span>`).join('')}</div></div>`;
  }
  $('summary-title').textContent = `Summary · ${selectionLabel()}`;
  $('summary').innerHTML = html;
}

// One row per Monday-to-Sunday week, newest first.
function renderWeekly(rows, channels) {
  const mon = thisMonday();
  const weeks = [];
  for (let i = 0; i < WEEKS_SHOWN; i++) {
    const from = addDays(mon, -7 * i);
    const to = addDays(from, 6);
    weeks.push({ from, to, use: usageBetween(rows, from, to, channels) });
  }
  const multi = channels.length > 1;
  $('weekly-title').textContent = `Weekly usage (Monday to Sunday) · ${selectionLabel()}`;
  $('weekly').innerHTML = `<table>
    <thead><tr><th>Week</th>${channels.map((c) => `<th class="num">${esc(c)}</th>`).join('')}${multi ? '<th class="num">Total</th>' : ''}</tr></thead>
    <tbody>${weeks.map((w, i) => `<tr${i === 0 ? ' class="current"' : ''}><td>${fmtRange(w.from, w.to)}${i === 0 ? ' <span class="muted small">(this week, so far)</span>' : ''}</td>${channels.map((c) => `<td class="num">${fmtL(w.use[c])}</td>`).join('')}${multi ? `<td class="num"><b>${fmtL(sumOf(w.use))}</b></td>` : ''}</tr>`).join('')}</tbody>
  </table>`;
}

function renderDaily(series, channels, range) {
  const seriesDef = channels.map((c) => ({ key: c, label: c, cssVar: channelVar(c) }));
  const oneWeek = series.length <= 7;
  const tick = (key, short) => (short ? (oneWeek ? fmtWeekday(key) : fmtDay(key, true)) : fmtDay(key));
  state.chartArgs = {
    rows: series, series: seriesDef, formatValue: fmtAxis, formatDay: tick,
    onPick: (r) => { pickDay(r.day); $('dayview-title').scrollIntoView({ behavior: 'smooth', block: 'start' }); },
  };
  $('daily-title').textContent = `Daily usage · ${selectionLabel()}`;
  $('legend').innerHTML = channels.length > 1 ? channels.map((c) => `<span>${swatch(c)}${esc(c)}</span>`).join('') : '';
  renderStackedBars($('chart'), state.chartArgs);

  const sums = Object.fromEntries(channels.map((c) => [c, series.reduce((s, r) => s + r[c], 0)]));
  const all = Object.values(sums).reduce((a, b) => a + b, 0);
  $('period-total').textContent = `${PERIODS[state.period].label} (${fmtRange(range.from, range.to)}): ${fmtL(all)}` +
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
    return `<span class="pair">${swatch(name)}${field === 'pulses' ? fmtCount(v) : v == null ? '–' : fmtL(v)}</span>`;
  }).join('');
  $('recent').innerHTML = `<table>
    <thead><tr><th>Time</th><th>Meter</th><th>Totals</th><th>Counter (7111)</th><th>Used since previous</th></tr></thead>
    <tbody>${list.map((r) => `<tr>
      <td>${dtFmt.format(new Date(r.created_at))}</td><td>${esc(names.get(r.device_id) || r.device_id)}</td>
      <td>${cell(r.ch, 'litres')}</td><td>${cell(r.ch, 'pulses')}</td><td>${cell(r.ch, 'used_l')}</td></tr>`).join('')}</tbody>
  </table>`;
}

// ---------------------------------------------------------------------------
// One day, hour by hour
// Each reading's usage (since the previous reading) goes into the hour the
// reading was sent, so the detail depends on how often the meter sends.
// ---------------------------------------------------------------------------
const hourOf = (iso) => Number(new Intl.DateTimeFormat('en-GB', { timeZone: TIME_ZONE, hour: '2-digit', hourCycle: 'h23' }).format(new Date(iso)));
const hourLabel = (h) => `${String(h).padStart(2, '0')}:00`;

async function loadDay() {
  const day = state.day || todayKey();
  state.day = day;
  $('day').value = day;
  $('day').max = todayKey();
  $('day-next').disabled = day >= todayKey();
  const ids = selectedDevices().map((d) => d.id);
  const channels = state.channels;
  $('dayview-title').textContent = `During the day · ${selectionLabel()}`;
  if (!ids.length) { $('day-chart').innerHTML = '<p class="empty">No meters selected.</p>'; return; }
  try {
    // Irish midnight is 23:00 or 00:00 UTC: load a little either side, then keep the Irish day.
    const since = `${addDays(day, -1)}T22:00:00Z`;
    const until = `${addDays(day, 1)}T02:00:00Z`;
    const rows = [];
    for (let offset = 0; ; offset += 1000) {
      const page = await api.channelUsage({ ids, since, until, limit: 1000, offset });
      rows.push(...page);
      if (page.length < 1000) break;
    }
    if (state.day !== day) return;  // another day was picked meanwhile
    const mine = rows.filter((r) => dayKey(new Date(r.created_at)) === day);
    const hours = Array.from({ length: 24 }, (_, h) => Object.fromEntries([['day', h], ...channels.map((c) => [c, 0])]));
    for (const r of mine) {
      const t = hours[hourOf(r.created_at)];
      if (t && r.channel in t) t[r.channel] += Number(r.used_l) || 0;
    }
    const seriesDef = channels.map((c) => ({ key: c, label: c, cssVar: channelVar(c) }));
    state.dayArgs = {
      rows: hours, series: seriesDef, formatValue: fmtAxis,
      formatDay: (h, short) => (short ? hourLabel(h) : `${fmtShort(day)}, ${hourLabel(h)}–${hourLabel((h + 1) % 24)}`),
      ariaLabel: 'Water usage per hour, stacked by inlet',
    };
    $('day-legend').innerHTML = channels.length > 1 ? channels.map((c) => `<span>${swatch(c)}${esc(c)}</span>`).join('') : '';
    renderStackedBars($('day-chart'), state.dayArgs);

    const sums = Object.fromEntries(channels.map((c) => [c, hours.reduce((a, r) => a + r[c], 0)]));
    const readings = new Set(mine.map((r) => r.reading_id)).size;
    $('day-total').textContent = `${fmtDay(day)}: ${fmtL(sumOf(sums))}` +
      (channels.length > 1 ? ` (${channels.map((c) => `${c.toLowerCase()} ${fmtL(sums[c])}`).join(', ')})` : '') +
      ` · ${readings} reading${readings === 1 ? '' : 's'}. Usage is counted in the hour each reading was sent.`;

    const multi = channels.length > 1;
    const used = hours.filter((r) => channels.some((c) => r[c] > 0));
    $('day-table').innerHTML = used.length ? `<table>
      <thead><tr><th>Hour</th>${channels.map((c) => `<th class="num">${esc(c)}</th>`).join('')}${multi ? '<th class="num">Total</th>' : ''}</tr></thead>
      <tbody>${used.map((r) => `<tr><td>${hourLabel(r.day)}–${hourLabel((r.day + 1) % 24)}</td>${channels.map((c) => `<td class="num">${fmtL(r[c])}</td>`).join('')}${multi ? `<td class="num">${fmtL(channels.reduce((a, c) => a + r[c], 0))}</td>` : ''}</tr>`).join('')}</tbody>
    </table>` : '<p class="empty">No water used on this day.</p>';
  } catch (err) {
    console.error(err);
    $('day-chart').innerHTML = `<p class="empty">Could not load this day (${esc(err.message)}).</p>`;
  }
}

function pickDay(key) {
  if (!key) return;
  state.day = key > todayKey() ? todayKey() : key;
  loadDay();
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
    const mon = thisMonday();
    const [totals, weekRows, daily, recent] = await Promise.all([
      api.latestTotals(allIds),
      api.dailyUsage({ fromDay: mon, ids: allIds }),
      ids.length ? api.dailyUsage({ fromDay: addDays(mon, -7 * (WEEKS_SHOWN - 1)), ids }) : [],
      ids.length ? api.channelUsage({ ids, limit: 300 }) : [],
    ]);
    const channels = sortChannels(new Set(totals.filter((t) => ids.includes(t.device_id)).map((t) => t.channel)));
    const range = periodRange(state.period);
    renderDevices(totals, weekRows);
    renderSummary(daily, totals, ids, channels);
    state.channels = channels;
    renderDaily(dailySeries(daily, range.from, range.to, channels), channels, range);
    renderWeekly(daily, channels);
    renderRecent(recent);
    loadDay();
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
    const since = new Date(`${addDays(periodRange(state.period).from, -1)}T23:00:00Z`).toISOString();  // covers Irish midnight in summer and winter
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
      return [local.format(new Date(r.created_at)), d.name, d.kind, r.channel, r.litres, r.pulses, r.used_l].map(cell).join(',');
    });
    const csv = ['time,meter,kind,channel,total_l,counter_pulses,used_l', ...lines].join('\n');
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
  state.period = b.dataset.period;
  load();
}));
$('toggle-table').addEventListener('click', (e) => {
  const showTable = $('daily-table').hidden;
  $('daily-table').hidden = !showTable;
  $('chart').hidden = showTable;
  e.target.textContent = showTable ? 'Show as chart' : 'Show as table';
});
$('day').addEventListener('change', (e) => pickDay(e.target.value));
$('day-prev').addEventListener('click', () => pickDay(addDays(state.day || todayKey(), -1)));
$('day-next').addEventListener('click', () => pickDay(addDays(state.day || todayKey(), 1)));
$('toggle-day-table').addEventListener('click', (e) => {
  const showTable = $('day-table').hidden;
  $('day-table').hidden = !showTable;
  $('day-chart').hidden = showTable;
  e.target.textContent = showTable ? 'Show as chart' : 'Show as table';
});
let resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    if (state.chartArgs) renderStackedBars($('chart'), state.chartArgs);
    if (state.dayArgs) renderStackedBars($('day-chart'), state.dayArgs);
  }, 200);
});
setInterval(load, 5 * 60 * 1000);
load();
