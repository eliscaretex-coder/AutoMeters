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

const inIds = (ids) => (ids && ids.length ? `in.(${ids.join(',')})` : undefined);

export const listDevices = () =>
  get('devices', { select: '*', order: 'sort_order.asc,name.asc' });

export const latestTotals = (ids) =>
  get('latest_totals', { select: '*', device_id: inIds(ids) });

export const dailyUsage = ({ fromDay, ids }) =>
  get('daily_usage', { select: 'day,device_id,channel,used_l', day: `gte.${fromDay}`, device_id: inIds(ids), order: 'day.asc' });

export const channelUsage = ({ ids, since, limit = 300, offset = 0 }) =>
  get('channel_usage', {
    select: 'reading_id,created_at,device_id,channel,litres,used_l',
    device_id: inIds(ids),
    created_at: since ? `gte.${since}` : undefined,
    order: 'created_at.desc,reading_id.desc',
    limit,
    offset: offset || undefined,
  });
