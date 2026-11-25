# AutoMeters

Water meter readers for a laundry: 9 washing machines (Cold, Warm and Hot inlets each), the
boiler meter and the canteen meter (one input each). Each reader (today an M5Stack AtomS3R)
counts the meter pulses, shows the totals on its screen and sends them to Supabase. A web page
on GitHub Pages shows the readings.

```
Atom (firmware/) --HTTPS--> Supabase: rpc/submit_reading --> devices, readings, reading_channels
                                                                   |
                    GitHub Pages (docs/) <-- read-only, publishable key
```

## Folders

| Folder | What is in it |
|---|---|
| `firmware/atoms3r_water_meter/` | Arduino sketch for the AtomS3R (pulse counting, screen, settings page, sending). |
| `firmware/xiao_c6_water_meter/` | Same for the Seeed XIAO ESP32-C6 on battery: deep sleep between pulses, Wi-Fi only to send. Bench test guide (Portuguese): `TESTE_BANCADA.md`. |
| `supabase/migrations/` | SQL that creates the database: tables, Row Level Security, views and the `submit_reading` function. |
| `docs/` | The web page (plain HTML/JS, no build step). Served by GitHub Pages. |
| `GUIDE.md` | Installation guide: wiring, Supabase set-up, flashing, on-site set-up, troubleshooting. |

## Security

- The Atoms and the web page only use the Supabase **publishable** key. It is public by design.
- Nobody can write to the tables directly. The Atoms call `submit_reading()`, which checks a
  secret **ingest token** (stored hashed in a schema the API does not expose).
- Readings are readable by anyone who has the page's key. To make the page login-only later,
  change the two `read` policies in the SQL to `to authenticated` and add Supabase Auth to the page.
- Never commit the `secret` / `service_role` key or the ingest token. `firmware/**/secrets.h`
  is in `.gitignore` for that reason.

## Setting up

1. **Database:** run `supabase/migrations/20261120000000_water_meter.sql` in the Supabase SQL
   Editor, then `select private.create_ingest_token('Laundry Atoms');` and keep the token.
2. **Web page:** put the publishable key in `docs/assets/config.js`. In GitHub, go to
   **Settings > Pages**, choose **Deploy from a branch**, branch `main`, folder `/docs`.
3. **Atoms:** flash `firmware/atoms3r_water_meter` and type the key and token on the settings
   page (or build them in with `secrets.h`). See `GUIDE.md`.

## Adding features

- New meters: nothing to change in the database. A device appears on the page after its first
  send; its inputs are whatever `channels` it sends (`[{name: "Water", litres, pulses}]` for a
  single meter). Send `"kind": "boiler"` / `"canteen"` or set `devices.kind` by hand.
- New data from the devices: add it to the JSON the firmware sends. `submit_reading` already
  stores the whole message in `readings.payload`, so it is kept even before it gets its own column.
- New database changes: add a new file in `supabase/migrations/` (never edit one that was already run).
- New page sections: `docs/assets/api.js` has the queries, `app.js` the rendering, `chart.js` the chart.
