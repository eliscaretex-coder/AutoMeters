// Small read-only client for the Supabase REST API (PostgREST).
// No library needed: every call is a plain GET with the publishable key.
import { SUPABASE_URL, SUPABASE_KEY } from './config.js';

export const isConfigured = () => Boolean(SUPABASE_URL && SUPABASE_KEY);

function headers() {
  const h = { apikey: SUPABASE_KEY, Accept: 'application/json' };
  // Legacy anon keys are JWTs and also go in Authorization.
  if (SUPABASE_KEY.startsWith('eyJ')) h.Authorization = `Bearer ${SUPABASE_KEY}`;
  return h;
}

async function get(path, params = {}) {
  const url = new URL(`${SUPABASE_URL.replace(/\/+$/, '')}/rest/v1/${path}`);
  for (const [k, v] of Object.entries(params)) {
    if (v !== undefined && v !== null && v !== '') url.searchParams.append(k, v);
  }
  const res = await fetch(url, { headers: headers() });
  if (!res.ok) {
    let detail = '';
    try { detail = (await res.json()).message || ''; } catch { /* not JSON */ }
    throw new Error(`Supabase ${res.status}${detail ? `: ${detail}` : ''}`);
  }
  return res.json();
}

// PostgREST returns at most 1000 rows per request: page through bigger results.
async function getAll(path, params) {
  const rows = [];
  for (let offset = 0; ; offset += 1000) {
    const page = await get(path, { ...params, limit: 1000, offset: offset || undefined });
    rows.push(...page);
    if (page.length < 1000) return rows;
  }
}

const inIds = (ids) => (ids && ids.length ? `in.(${ids.join(',')})` : undefined);

export const listDevices = () =>
  get('devices', { select: '*', order: 'sort_order.asc,name.asc' });

export const latestTotals = (ids) =>
  get('latest_totals', { select: '*', device_id: inIds(ids) });

// Usage per day (Irish time) from fromDay on, oldest first.
export const dailyUsage = ({ fromDay, ids }) =>
  getAll('daily_usage', { select: 'day,device_id,channel,used_l', day: `gte.${fromDay}`, device_id: inIds(ids), order: 'day.asc,device_id.asc,channel.asc' });

export const channelUsage = ({ ids, since, limit = 300, offset = 0 }) =>
  get('channel_usage', {
    select: 'reading_id,created_at,device_id,channel,litres,pulses,used_l',
    device_id: inIds(ids),
    created_at: since ? `gte.${since}` : undefined,
    order: 'created_at.desc,reading_id.desc',
    limit,
    offset: offset || undefined,
  });
