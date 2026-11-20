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

export const listDevices = () =>
  get('devices', { select: '*', order: 'machine.asc' });

export async function latestReading(deviceId) {
  const rows = await get('readings', {
    select: 'created_at,cold_l,warm_l,hot_l,cold_pulses,warm_pulses,hot_pulses,rssi,fw',
    device_id: `eq.${deviceId}`,
    order: 'created_at.desc',
    limit: 1,
  });
  return rows[0] || null;
}

export const dailyUsage = ({ fromDay, deviceId }) =>
  get('daily_usage', {
    select: '*',
    day: `gte.${fromDay}`,
    device_id: deviceId ? `eq.${deviceId}` : undefined,
    order: 'day.asc',
  });

export const recentReadings = ({ deviceId, since, limit = 50, offset = 0 }) =>
  get('reading_usage', {
    select: '*',
    device_id: deviceId ? `eq.${deviceId}` : undefined,
    created_at: since ? `gte.${since}` : undefined,
    order: 'created_at.desc',
    limit,
    offset: offset || undefined,
  });
