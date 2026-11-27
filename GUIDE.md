# Water meter reader with AtomS3R: installation guide

One AtomS3R per machine reads all three water inlets (Cold, Warm and Hot). It shows the total for each one on its screen and sends the readings to a Supabase database. A web page (GitHub Pages) shows the readings and can export them as a CSV file for Excel.

The Atom listens to the same magnet contact that already drives each Trumeter 7111 counter. The 7111s keep working as before.

---

## 1. Wiring

### Standard 7111 counter (battery powered, contact input)

The 7111 holds its input high through a very weak internal resistor (3.3 MΩ, about 3 V). That is too weak to feed the Atom directly, so the Atom uses its own pull-up and "shares" the contact through a small diode. Wire each of the three meters like this:

| AtomS3R | Part | 7111 counter |
|---|---|---|
| G5 (Cold), G6 (Warm), G7 (Hot) | **1N4148 diode**: plain end (anode) on the Atom, **striped end (cathode) on the 7111** | terminal 2 (Low Speed Count Input) |
| GND | wire | terminal 6 (0 V, Common) |

- Keep the meter's reed contact on terminals 2 and 6 as it is now.
- On the settings page, leave the wiring as **"Internal pull-up: diode to the 7111, optocoupler or bare sensor"** (the default).
- When the contact closes, it pulls both the 7111 and the Atom low, so both count the same pulse.
- If the Atom is switched off, the diode blocks, and the 7111 keeps counting on its own as before.
- A 1N4148 costs cents. A Schottky diode such as a BAT43 or BAT85 also works.
- Your 2.37 V reading is normal: the multimeter itself loads the 3.3 MΩ resistor. The real voltage is closer to 3 V.
- Keep the Atom's wires short, or use screened cable if they run near motors or inverters.

### 7111HV counter (10 to 240 V input)

Never connect the Atom directly in this case. Use an **optocoupler module** whose input is rated for your panel voltage (for example a 24 V PC817 module or a 230 V AC module):

- The module's input goes in parallel with the 7111HV input.
- The module's output goes like this: collector to the Atom GPIO, emitter to the Atom GND.
- In the settings, choose **"Straight to the sensor or via optocoupler (internal pull-up)"** for that inlet.

Each inlet has its own settings, so you can mix both counter types on the same machine.

### Power

Power the Atom from a 5 V USB-C charger or from the 5V and GND pins. Mount it in the panel, away from water.

---

### Machine running signal (XIAO readers)

A dry relay contact that closes while the machine runs goes straight between **D3** and **GND**
of the XIAO (internal pull-up, no external voltage: the pins take 3.3 V at most). The XIAO sends
each start and stop at once; the web page shows the periods in **During the day**. XIAOs fed
from the machine's 5 V (Power mode "Always on") say "still here" to the Atom every minute, so a
machine switched off at the wall shows as **Switched off**. Run
`supabase/migrations/20261127000000_machine_running.sql` once in the SQL Editor for this.

## 2. Supabase (once only, shared by all machines)

1. Open the Supabase project, then **SQL Editor > New query**.
2. Paste the whole file `supabase/migrations/20261120000000_water_meter.sql` and click **Run**. It creates the tables, the security rules and the `submit_reading` function the Atoms call.
3. Still in the SQL Editor, create the token the Atoms will use:
   ```sql
   select private.create_ingest_token('Laundry Atoms');
   ```
   Copy the value it returns. It is shown only once (only a hash is stored). If it leaks, revoke it with
   `update private.ingest_tokens set revoked = true where label = 'Laundry Atoms';` and create a new one.
4. Go to **Project Settings > API Keys** and copy the **publishable key** (`sb_publishable_...`). This key is public by design: it can only read readings and call `submit_reading`. Never put the `secret` / `service_role` key on an Atom or the web page.

---

## 3. Flashing the firmware (Arduino IDE)

1. In **Tools > Manage Libraries**, install:
   - **M5Unified** (it also installs M5GFX). Use the latest version: recent AtomS3R boards have a new screen chip.
   - **ArduinoJson** (by Benoit Blanchon), version 7.
2. Open `firmware/atoms3r_water_meter/atoms3r_water_meter.ino`. The `supabase_roots.h` file must stay in the same folder.
   - Optional: copy `secrets.example.h` to `secrets.h` and fill in the publishable key and the ingest token. Every Atom flashed this way already knows them, so on site you only type the Wi-Fi. `secrets.h` is never pushed to GitHub.
3. Pick the same board you used for the examples (AtomS3R / AtomS3), and set **USB CDC On Boot: Enabled** so the Serial Monitor works.
4. Click **Upload**.
   - If it isn't recognised, hold the Atom's reset button (on the side) for about 2 s until the green LED lights, then try again.
5. Optional: the **Serial Monitor** at 115200 shows the IP address and any send errors.

---

## 4. Set-up at installation

1. On first power-up, the screen shows **SETUP MODE** and the name of a Wi-Fi network, for example `WaterMeter-AB12` (password `watermeter`).
2. Connect your phone to that network. The settings page opens by itself (or open `http://192.168.4.1`).
3. Fill in:
   - **Machine name**, as it should appear on the web page (e.g. "Machine 03").
   - **Wi-Fi network and password** for the site.
   - **Supabase project URL** (already filled in), **publishable key** and **ingest token** (from step 2). Leave them blank if they were built in with `secrets.h`.
   - For each inlet (Cold, Warm, Hot): in use or not, pin, the meter's **litres per pulse**, and the wiring type.
   - **Password for this page** (the default is `watermeter`; change it).
4. Click **Save and restart**. Once it connects, the screen shows its IP address for 5 seconds.
5. To make the Atom start from the same numbers as the counters, open the page again (`http://<ip>/`, user `admin`). Under **"Match the count to the 7111"**, type the number shown on each 7111.
6. Click **Send a reading now** and check that the machine appears on the web page.

To get back into SETUP mode later, **hold the screen (the button) for 5 seconds**. Counting carries on during set-up.

---

## 5. Day-to-day use

- **Screen:** machine name and, for **C**old, **W**arm and **H**ot:
  - top line, large: litres used **this week** (Monday 00:00 to Sunday 24:00, Irish time). It comes from Supabase, so it is the same number as the web page and survives restarts. It shows `--` until the Atom has the time and has sent its first reading.
  - bottom line: the **total** litres (accumulated) on the left and, in amber on the right, the **counter**: the number of pulses, to compare with the number on the 7111.
  - A dot flashes for every pulse received.
- **Gateway screen (no meters on the Atom):** the first page lists the machines with the litres used this week. Tap the screen to see one machine at a time with the same three numbers per inlet.
- **Screen off:** the screen turns off after 60 s without a press (change it in the settings; 0 = always on). Press it once to wake it up. Counting and sending carry on with the screen off.
- **Footer:** time of the last send in green, or the error in orange (`no Wi-Fi`, `no API key`, `wrong token`, `error -1`...).
- It sends every 60 minutes (this can be changed). Because the Atom always sends the **running total**, nothing is lost if the Wi-Fi drops: the usage for that period goes into the next send.
- The count is saved every 30 s. In a power cut, at most the last 30 s of pulses are lost.

### What is stored

- **devices:** one row per meter reader (by MAC address): name, type (`washer`, `boiler`, `canteen`, `other`) and when it was last seen.
- **readings:** one row per send: date and time, Wi-Fi signal, firmware, and the full message the device sent.
- **reading_channels:** one row per meter input in each send, with its running total in litres and pulses. A washing machine has three inputs (Cold, Warm, Hot); the boiler and canteen meters have one (Water).
- **channel_usage**, **daily_usage** and **latest_totals** (views): usage since the previous send, usage per day (Irish time) and the latest total of each input. The web page uses these.

To change a meter's type or its order on the page, edit it in the Supabase Table Editor (table `devices`, columns `kind` and `sort_order`), or in SQL:
```sql
update devices set kind = 'boiler', sort_order = 50 where name = 'Boiler';
```

### Web page

The page in `docs/` shows every meter grouped by type (washing machines, boiler, canteen) with its totals and today's usage, a daily chart for 7/30/90 days (all washing machines together, or any single meter: click its card) and the latest readings. **Download CSV** gives the readings of the chosen period, one row per meter input, ready for a pivot table in Excel. It refreshes every 5 minutes.

---

## 6. Common problems

| Symptom | What to check |
|---|---|
| Atom counts fewer than the 7111 | Lower the debounce filter (e.g. 15 ms) and check the shared GND. |
| Atom counts too many (double pulses) | Raise the debounce filter (e.g. 50 ms) and use screened cable. |
| An inlet never counts | Check the diode direction: the striped end goes to the 7111 terminal 2. Check the wiring option is "Internal pull-up". |
| `wrong token` | The token on the Atom must be one created with `private.create_ingest_token` and not revoked. |
| `wrong API key` | Use the publishable key from Project Settings > API Keys. |
| `run SQL setup` | The `submit_reading` function does not exist yet: run the SQL file (section 2). |
| `error -1` | No connection to Supabase: check the project URL and that the site's Wi-Fi reaches the internet. |
