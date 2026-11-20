-- =============================================================================
--  AutoMeters: water meter readings
--
--  Run this once in the Supabase dashboard: SQL Editor > New query > paste > Run.
--
--  Security model
--  - The Atoms and the web page only ever use the PUBLISHABLE (anon) key.
--  - Nobody can write to the tables directly. The Atoms send readings through
--    the function public.submit_reading(), which checks a secret ingest token.
--    Tokens are stored only as SHA-256 hashes in the "private" schema, which is
--    not exposed by the API.
--  - Anyone with the publishable key can READ readings and devices (the web
--    page needs this). To make the page login-only later, change the two
--    "read" policies from "to anon, authenticated" to "to authenticated".
-- =============================================================================

create extension if not exists pgcrypto with schema extensions;

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;

-- ---------------------------------------------------------------------------
-- Tables
-- ---------------------------------------------------------------------------

-- One row per Atom (identified by its Wi-Fi MAC address).
create table public.devices (
  id          bigint generated always as identity primary key,
  mac         text not null unique,
  machine     text not null,
  fw          text,
  rssi        smallint,
  last_seen   timestamptz,
  created_at  timestamptz not null default now()
);
comment on table public.devices is 'Water meter units (AtomS3R or later boards).';

-- One row per send. Totals are running counters, exactly as the device keeps them.
create table public.readings (
  id           bigint generated always as identity primary key,
  created_at   timestamptz not null default now(),
  device_id    bigint not null references public.devices (id) on delete cascade,
  machine      text not null,
  cold_l       numeric(14, 3),
  warm_l       numeric(14, 3),
  hot_l        numeric(14, 3),
  cold_pulses  bigint,
  warm_pulses  bigint,
  hot_pulses   bigint,
  rssi         smallint,
  fw           text,
  boot_id      bigint,
  seq          integer,
  uptime_s     bigint,
  payload      jsonb not null   -- everything the device sent (minus the token), for future use
);
comment on table public.readings is 'Raw readings sent by each device. Totals in litres are cumulative.';

create index readings_device_time_idx on public.readings (device_id, created_at desc);
create index readings_time_idx on public.readings (created_at desc);

-- Ingest tokens (hashed). Not reachable through the API.
create table private.ingest_tokens (
  id          bigint generated always as identity primary key,
  label       text not null,
  token_hash  text not null unique,
  revoked     boolean not null default false,
  created_at  timestamptz not null default now(),
  last_used   timestamptz
);

-- ---------------------------------------------------------------------------
-- Row Level Security: read-only for the API, no direct writes
-- ---------------------------------------------------------------------------
alter table public.devices enable row level security;
alter table public.readings enable row level security;

revoke insert, update, delete, truncate on public.devices, public.readings from anon, authenticated;
grant select on public.devices, public.readings to anon, authenticated;

create policy "devices are readable" on public.devices
  for select to anon, authenticated using (true);

create policy "readings are readable" on public.readings
  for select to anon, authenticated using (true);

-- ---------------------------------------------------------------------------
-- Views used by the web page (security_invoker: they respect RLS above)
-- ---------------------------------------------------------------------------

-- Usage in litres since the previous reading of the same device.
-- A negative difference (count adjusted or reset) is reported as null.
create view public.reading_usage
with (security_invoker = true) as
select
  d.id, d.created_at, d.device_id, d.machine,
  d.cold_l, d.warm_l, d.hot_l,
  case when d.cold_diff >= 0 then d.cold_diff end as cold_used_l,
  case when d.warm_diff >= 0 then d.warm_diff end as warm_used_l,
  case when d.hot_diff  >= 0 then d.hot_diff  end as hot_used_l,
  d.rssi, d.fw
from (
  select
    r.*,
    r.cold_l - lag(r.cold_l) over w as cold_diff,
    r.warm_l - lag(r.warm_l) over w as warm_diff,
    r.hot_l  - lag(r.hot_l)  over w as hot_diff
  from public.readings r
  window w as (partition by r.device_id order by r.created_at, r.id)
) d;

-- Usage per day (Irish time) and per device.
create view public.daily_usage
with (security_invoker = true) as
select
  (u.created_at at time zone 'Europe/Dublin')::date as day,
  u.device_id,
  max(u.machine)                         as machine,
  coalesce(sum(u.cold_used_l), 0)        as cold_l,
  coalesce(sum(u.warm_used_l), 0)        as warm_l,
  coalesce(sum(u.hot_used_l), 0)         as hot_l,
  coalesce(sum(u.cold_used_l), 0)
    + coalesce(sum(u.warm_used_l), 0)
    + coalesce(sum(u.hot_used_l), 0)     as total_l,
  count(*)                               as readings
from public.reading_usage u
group by 1, 2;

grant select on public.reading_usage, public.daily_usage to anon, authenticated;

-- ---------------------------------------------------------------------------
-- Ingest function called by the devices
--   POST /rest/v1/rpc/submit_reading   body: {"payload": { ...device JSON... }}
-- ---------------------------------------------------------------------------
create or replace function public.submit_reading(payload jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_token_id  bigint;
  v_mac       text;
  v_machine   text;
  v_device_id bigint;
  v_ch        jsonb;
  v_litres    numeric[] := array[null, null, null]::numeric[];
  v_pulses    bigint[]  := array[null, null, null]::bigint[];
  v_name      text;
  v_idx       int;
begin
  if payload is null or jsonb_typeof(payload) <> 'object' or pg_column_size(payload) > 8192 then
    return jsonb_build_object('ok', false, 'error', 'payload');
  end if;

  select t.id into v_token_id
  from private.ingest_tokens t
  where t.token_hash = encode(extensions.digest(coalesce(payload ->> 'token', ''), 'sha256'), 'hex')
    and not t.revoked;
  if v_token_id is null then
    return jsonb_build_object('ok', false, 'error', 'token');
  end if;

  v_mac := left(coalesce(nullif(trim(payload ->> 'device'), ''), 'unknown'), 32);
  v_machine := left(coalesce(nullif(trim(payload ->> 'machine'), ''), 'unnamed'), 40);

  -- Channels: matched by name (Cold / Warm / Hot), falling back to position.
  if jsonb_typeof(payload -> 'channels') = 'array' then
    for v_idx in 0 .. least(jsonb_array_length(payload -> 'channels'), 3) - 1 loop
      v_ch := payload -> 'channels' -> v_idx;
      v_name := lower(coalesce(v_ch ->> 'name', ''));
      if v_name not in ('cold', 'warm', 'hot') then
        v_name := (array['cold', 'warm', 'hot'])[v_idx + 1];
      end if;
      if coalesce((v_ch ->> 'enabled')::boolean, true) then
        v_litres[array_position(array['cold', 'warm', 'hot'], v_name)] := round((v_ch ->> 'litres')::numeric, 3);
        v_pulses[array_position(array['cold', 'warm', 'hot'], v_name)] := (v_ch ->> 'pulses')::bigint;
      end if;
    end loop;
  end if;

  insert into public.devices as d (mac, machine, fw, rssi, last_seen)
  values (v_mac, v_machine, left(payload ->> 'fw', 20), (payload ->> 'rssi')::smallint, now())
  on conflict (mac) do update
    set machine = excluded.machine, fw = excluded.fw, rssi = excluded.rssi, last_seen = excluded.last_seen
  returning d.id into v_device_id;

  insert into public.readings (
    device_id, machine,
    cold_l, warm_l, hot_l, cold_pulses, warm_pulses, hot_pulses,
    rssi, fw, boot_id, seq, uptime_s, payload)
  values (
    v_device_id, v_machine,
    v_litres[1], v_litres[2], v_litres[3], v_pulses[1], v_pulses[2], v_pulses[3],
    (payload ->> 'rssi')::smallint, left(payload ->> 'fw', 20),
    (payload ->> 'boot')::bigint, (payload ->> 'seq')::int, (payload ->> 'uptime_s')::bigint,
    payload - 'token');

  update private.ingest_tokens set last_used = now() where id = v_token_id;

  return jsonb_build_object('ok', true);
exception
  when invalid_text_representation or numeric_value_out_of_range then
    return jsonb_build_object('ok', false, 'error', 'payload');
end;
$$;

revoke execute on function public.submit_reading(jsonb) from public;
grant execute on function public.submit_reading(jsonb) to anon, authenticated;

-- ---------------------------------------------------------------------------
-- Helper to create an ingest token (run from the SQL Editor only).
--   select private.create_ingest_token('Laundry Atoms');
-- It returns the token ONCE. Type it into each Atom's settings page.
-- ---------------------------------------------------------------------------
create or replace function private.create_ingest_token(p_label text)
returns text
language plpgsql
set search_path = ''
as $$
declare
  v_token text := encode(extensions.gen_random_bytes(18), 'hex');
begin
  insert into private.ingest_tokens (label, token_hash)
  values (p_label, encode(extensions.digest(v_token, 'sha256'), 'hex'));
  return v_token;
end;
$$;

revoke execute on function private.create_ingest_token(text) from public, anon, authenticated;
