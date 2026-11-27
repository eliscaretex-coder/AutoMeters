-- =============================================================================
--  Machine running: when each washing machine worked and when it stood still.
--
--  The XIAO readers have a "machine running" input (1 = running, 0 = stopped).
--  Each change goes through the Atom to submit_reading() inside the reading:
--    "run": {"active": true,
--            "events": [{"n": 12, "on": true, "ago_s": 3, "power_up": false}, ...]}
--  ago_s is how long ago the change happened (the reader has no clock), so the
--  time stored is the time of arrival minus ago_s.
--
--  Switched off: the XIAOs are fed from the machine, so they go off with it.
--  They say "still here" to the Atom every minute; when one goes silent the
--  Atom sends an event {"power_off": true} dated the last time it heard it
--  (payload "report": "power_off", which does not count as a reading). The
--  reader's power-up event ends that period. If the Atom missed it (it was
--  restarted too), the power-up adds the "off" from the reader's last reading.
--
--  - machine_events:   one row per change (a resend of the same change is
--                      ignored: device + boot + n is unique).
--  - machine_periods:  the events joined into periods: started_at, ended_at
--                      (null = still going) and state: 'running', 'stopped'
--                      or 'off' (machine switched off, no power).
--
--  Run once in the Supabase SQL Editor (after the earlier files). Safe to run again.
-- =============================================================================

create table if not exists public.machine_events (
  id           bigint generated always as identity primary key,
  device_id    bigint not null references public.devices (id) on delete cascade,
  at           timestamptz not null,           -- when the machine started / stopped
  active       boolean not null,               -- true = started, false = stopped
  power_up     boolean not null default false, -- state read when the reader was switched on
  power_off    boolean not null default false, -- machine switched off (reader went silent)
  boot_id      bigint,
  ev_n         integer,
  reading_id   bigint references public.readings (id) on delete set null,
  received_at  timestamptz not null default now(),
  unique (device_id, boot_id, ev_n)
);
alter table public.machine_events add column if not exists power_off boolean not null default false;
create index if not exists machine_events_device_time_idx on public.machine_events (device_id, at);
comment on table public.machine_events is 'Machine running input: one row per change (start / stop).';

alter table public.machine_events enable row level security;
revoke insert, update, delete, truncate on public.machine_events from anon, authenticated;
grant select on public.machine_events to anon, authenticated;
drop policy if exists "machine events are readable" on public.machine_events;
create policy "machine events are readable" on public.machine_events
  for select to anon, authenticated using (true);

-- Periods per device: consecutive events with the same state are one period.
drop view if exists public.machine_periods;
create view public.machine_periods
with (security_invoker = true) as
with e as (
  select m.device_id, m.at, m.id,
         case when m.power_off then 'off' when m.active then 'running' else 'stopped' end as state
  from public.machine_events m
), l as (
  select e.*, lag(e.state) over (partition by e.device_id order by e.at, e.id) as prev_state
  from e
), starts as (
  select * from l where prev_state is distinct from state
)
select
  s.device_id,
  s.state,
  s.state = 'running' as active,
  s.at as started_at,
  lead(s.at) over (partition by s.device_id order by s.at, s.id) as ended_at
from starts s;

grant select on public.machine_periods to anon, authenticated;

-- ---------------------------------------------------------------------------
-- submit_reading: same as before, plus the "run" events
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
  v_ev         jsonb;
  v_ago        bigint;
  v_report     text := payload ->> 'report';
  v_boot       bigint;
  v_up_at      timestamptz;
  v_last       timestamptz;
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
  v_boot := (payload ->> 'boot')::bigint;

  -- The Atom reports a silent reader as switched off: only the event, no reading
  if v_report = 'power_off' then
    select d.id into v_device_id from public.devices d where d.mac = v_mac;
    if v_device_id is null then
      return jsonb_build_object('ok', false, 'error', 'device');
    end if;
    for v_ev in select * from jsonb_array_elements(coalesce(payload -> 'run' -> 'events', '[]'::jsonb)) limit 4 loop
      v_ago := least(greatest(coalesce((v_ev ->> 'ago_s')::bigint, 0), 0), 30 * 86400);
      insert into public.machine_events (device_id, at, active, power_off, boot_id, ev_n, received_at)
      values (v_device_id, v_now - make_interval(secs => v_ago), false, true, v_boot, (v_ev ->> 'n')::int, v_now)
      on conflict (device_id, boot_id, ev_n) do nothing;
    end loop;
    update private.ingest_tokens set last_used = v_now where id = v_token_id;
    return jsonb_build_object('ok', true);
  elsif v_report is not null then
    return jsonb_build_object('ok', false, 'error', 'payload');
  end if;
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

  -- Machine running: one row per start / stop (a resend is ignored)
  if jsonb_typeof(payload -> 'run') = 'object' and jsonb_typeof(payload -> 'run' -> 'events') = 'array' then
    for v_ev in select * from jsonb_array_elements(payload -> 'run' -> 'events') limit 32 loop
      continue when v_ev ->> 'on' is null;
      v_ago := least(greatest(coalesce((v_ev ->> 'ago_s')::bigint, 0), 0), 30 * 86400);
      insert into public.machine_events (device_id, at, active, power_up, power_off, boot_id, ev_n, reading_id, received_at)
      values (v_device_id, v_now - make_interval(secs => v_ago), (v_ev ->> 'on')::boolean,
              coalesce((v_ev ->> 'power_up')::boolean, false), coalesce((v_ev ->> 'power_off')::boolean, false),
              v_boot, (v_ev ->> 'n')::int, v_reading_id, v_now)
      on conflict (device_id, boot_id, ev_n) do nothing;
      if coalesce((v_ev ->> 'power_up')::boolean, false) then
        v_up_at := v_now - make_interval(secs => v_ago);
      end if;
    end loop;

    -- The reader was just switched on and nobody reported it off (Atom restarted
    -- meanwhile): it was off since its last reading of the previous power-up.
    if v_up_at is not null then
      select max(r.created_at) into v_last
      from public.readings r
      where r.device_id = v_device_id and r.id <> v_reading_id
        and r.boot_id is distinct from v_boot and r.created_at < v_up_at;
      if v_last is not null and not exists (
        select 1 from public.machine_events m
        where m.device_id = v_device_id and m.power_off and m.at >= v_last and m.at <= v_up_at
      ) then
        insert into public.machine_events (device_id, at, active, power_off, boot_id, ev_n, reading_id, received_at)
        values (v_device_id, v_last, false, true, v_boot, 0, v_reading_id, v_now)
        on conflict (device_id, boot_id, ev_n) do nothing;
      end if;
    end if;
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
