-- =============================================================================
--  Do not count a counter adjustment as water used.
--
--  When the count is typed in to match the 7111 (or the litres per pulse is
--  raised), the running total jumps. channel_usage used to report that jump as
--  usage, so the week and the day showed the whole meter total.
--
--  A difference now counts as usage only when it is plausible: up to 100 L,
--  or up to 100 L per minute since the previous reading. Anything bigger is an
--  adjustment and is reported as null, like a count that went down.
--  daily_usage and the web page use this view, so they follow automatically.
--
--  Run once in the Supabase SQL Editor. Safe to run again.
-- =============================================================================
create or replace view public.channel_usage
with (security_invoker = true) as
select
  d.reading_id, d.created_at, d.device_id, d.channel, d.litres, d.pulses,
  case
    when d.diff >= 0
     and (d.diff <= 100
          or d.diff <= 100 * greatest(extract(epoch from d.created_at - d.prev_at) / 60.0, 1))
    then d.diff
  end as used_l
from (
  select c.*,
         c.litres - lag(c.litres) over w     as diff,
         lag(c.created_at) over w            as prev_at
  from public.reading_channels c
  window w as (partition by c.device_id, c.channel order by c.created_at, c.reading_id)
) d;

grant select on public.channel_usage to anon, authenticated;
