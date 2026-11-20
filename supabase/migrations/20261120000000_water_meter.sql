-- =============================================================================
--  AutoMeters: water meter readings
--
--  Run this once in the Supabase dashboard: SQL Editor > New query > paste > Run.
--
--  Data model
--  - devices:          one row per meter reader (washing machine, boiler, canteen...).
--  - readings:         one row per send (header: time, signal, firmware...).
--  - reading_channels: one row per meter input in that send (e.g. Cold / Warm / Hot
--                      on a washing machine, a single "Water" input on the boiler).
--  Any number of devices and any channel names work without schema changes.
--
--  Security model
--  - The devices and the web page only ever use the PUBLISHABLE (anon) key.
--  - Nobody can write to the tables directly. Devices send readings through
--    public.submit_reading(), which checks a secret ingest token. Tokens are
--    stored only as SHA-256 hashes in the "private" schema, which the API
--    does not expose.
--  - Anyone with the publishable key can READ the data (the web page needs
--    this). To make the page login-only later, change the "read" policies
--    from "to anon, authenticated" to "to authenticated".
-- =============================================================================

create extension if not exists pgcrypto with schema extensions;

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;

-- ---------------------------------------------------------------------------
-- Tables
-- ---------------------------------------------------------------------------

create table public.devices (
  id          bigint generated always as identity primary key,
  mac         text not null unique,           -- hardware id sent by the device
  name        text not null,                  -- e.g. "Machine 03", "Boiler"
  kind        text not null default 'washer'  -- washer | boiler | canteen | other
              check (kind in ('washer', 'boiler', 'canteen', 'other')),
  sort_order  integer not null default 100,   -- order on the web page
  fw          text,
  rssi        smallint,
  last_seen   timestamptz,
  created_at  timestamptz not null default now()
);
comment on table public.devices is 'Meter readers. kind and sort_order can be edited by hand.';

create table public.readings (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  device_id   bigint not null references public.devices (id) on delete cascade,
  rssi        smallint,
  fw          text,
  boot_id     bigint,
  seq         integer,
  uptime_s    bigint,
  payload     jsonb not null   -- everything the device sent (minus the token), for future use
);
create index readings_device_time_idx on public.readings (device_id, created_at desc);
create index readings_time_idx on public.readings (created_at desc);

create table public.reading_channels (
  reading_id  bigint not null references public.readings (id) on delete cascade,
  device_id   bigint not null references public.devices (id) on delete cascade,
  created_at  timestamptz not null,
  channel     text not null,            -- "Cold", "Warm", "Hot", "Water"...
  litres      numeric(14, 3),           -- running total, as kept by the device
  pulses      bigint,
  primary key (reading_id, channel)
);
create index reading_channels_series_idx on public.reading_channels (device_id, channel, created_at);
comment on table public.reading_channels is 'Running totals per meter input. Usage = difference between consecutive rows.';

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
alter table public.reading_channels enable row level security;

revoke insert, update, delete, truncate on public.devices, public.readings, public.reading_channels from anon, authenticated;
grant select on public.devices, public.readings, public.reading_channels to anon, authenticated;

create policy "devices are readable" on public.devices
  for select to anon, authenticated using (true);
create policy "readings are readable" on public.readings
  for select to anon, authenticated using (true);
create policy "reading channels are readable" on public.reading_channels
  for select to anon, authenticated using (true);

-- ---------------------------------------------------------------------------
-- Views used by the web page (security_invoker: they respect RLS above)
-- ---------------------------------------------------------------------------

-- Usage in litres since the previous reading of the same device and channel.
-- A negative difference (count adjusted or reset) is reported as null.
create view public.channel_usage
with (security_invoker = true) as
select
  d.reading_id, d.created_at, d.device_id, d.channel, d.litres, d.pulses,
  case when d.diff >= 0 then d.diff end as used_l
from (
  select c.*,
         c.litres - lag(c.litres) over (partition by c.device_id, c.channel order by c.created_at, c.reading_id) as diff
  from public.reading_channels c
) d;

-- Usage per day (Irish time), device and channel.
create view public.daily_usage
with (security_invoker = true) as
select
  (u.created_at at time zone 'Europe/Dublin')::date as day,
  u.device_id,
  u.channel,
  coalesce(sum(u.used_l), 0) as used_l,
  max(u.litres)              as total_l,
  count(*)                   as readings
from public.channel_usage u
group by 1, 2, 3;

-- Latest running total per device and channel.
create view public.latest_totals
with (security_invoker = true) as
select distinct on (c.device_id, c.channel)
  c.device_id, c.channel, c.litres, c.pulses, c.created_at
from public.reading_channels c
order by c.device_id, c.channel, c.created_at desc, c.reading_id desc;

grant select on public.channel_usage, public.daily_usage, public.latest_totals to anon, authenticated;

-- ---------------------------------------------------------------------------
-- Ingest function called by the devices
--   POST /rest/v1/rpc/submit_reading   body: {"payload": { ...device JSON... }}
--   payload: token, device (MAC), machine (name), kind (optional), fw, rssi,
--            boot, seq, uptime_s, channels: [{name, enabled, litres, pulses}]
-- ---------------------------------------------------------------------------
create or replace function public.submit_reading(payload jsonb)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_token_id   bigint;
  v_mac        text;
  v_name       text;
  v_kind       text;
  v_device_id  bigint;
  v_reading_id bigint;
  v_now        timestamptz := now();
  v_ch         jsonb;
  v_channel    text;
begin
  if payload is null or jsonb_typeof(payload) <> 'object' or pg_column_size(payload) > 16384 then
    return jsonb_build_object('ok', false, 'error', 'payload');
  end if;

  select t.id into v_token_id
  from private.ingest_tokens t
  where t.token_hash = encode(extensions.digest(coalesce(payload ->> 'token', ''), 'sha256'), 'hex')
    and not t.revoked;
  if v_token_id is null then
    return jsonb_build_object('ok', false, 'error', 'token');
  end if;

  v_mac  := left(coalesce(nullif(trim(payload ->> 'device'), ''), 'unknown'), 32);
  v_name := left(coalesce(nullif(trim(payload ->> 'machine'), ''), 'unnamed'), 40);
  v_kind := lower(coalesce(payload ->> 'kind', ''));
  if v_kind not in ('washer', 'boiler', 'canteen', 'other') then v_kind := null; end if;

  -- The kind sent by the device is only used when the device is first seen,
  -- so a kind set by hand in the table is kept.
  insert into public.devices as d (mac, name, kind, fw, rssi, last_seen)
  values (v_mac, v_name, coalesce(v_kind, 'washer'), left(payload ->> 'fw', 20), (payload ->> 'rssi')::smallint, v_now)
  on conflict (mac) do update
    set name = excluded.name, fw = excluded.fw, rssi = excluded.rssi, last_seen = excluded.last_seen
  returning d.id into v_device_id;

  insert into public.readings (created_at, device_id, rssi, fw, boot_id, seq, uptime_s, payload)
  values (v_now, v_device_id, (payload ->> 'rssi')::smallint, left(payload ->> 'fw', 20),
          (payload ->> 'boot')::bigint, (payload ->> 'seq')::int, (payload ->> 'uptime_s')::bigint,
          payload - 'token')
  returning id into v_reading_id;

  if jsonb_typeof(payload -> 'channels') = 'array' then
    for v_ch in select * from jsonb_array_elements(payload -> 'channels') limit 16 loop
      continue when not coalesce((v_ch ->> 'enabled')::boolean, true);
      v_channel := left(coalesce(nullif(trim(v_ch ->> 'name'), ''), 'Water'), 20);
      insert into public.reading_channels (reading_id, device_id, created_at, channel, litres, pulses)
      values (v_reading_id, v_device_id, v_now, v_channel,
              round((v_ch ->> 'litres')::numeric, 3), round((v_ch ->> 'pulses')::numeric)::bigint)
      on conflict do nothing;
    end loop;
  end if;

  update private.ingest_tokens set last_used = v_now where id = v_token_id;

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
-- It returns the token ONCE. Type it into each device's settings page.
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
