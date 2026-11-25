// Optional build-time defaults. Copy this file to "secrets.h" in the same
// folder and fill it in. secrets.h is listed in .gitignore, so it is never
// pushed to GitHub. Anything left blank here can be typed on the Atom's
// settings page instead.
#pragma once

// Supabase project URL (Project Settings > Data API)
#define SUPABASE_URL "https://obdltdjykzrxxpozupqt.supabase.co"

// Publishable key, "sb_publishable_..." (Project Settings > API Keys).
// It is safe to be public: Row Level Security only lets it read readings
// and call submit_reading().
#define SUPABASE_KEY ""

// Ingest token: the value returned by
//   select private.create_ingest_token('Laundry Atoms');
// Keep this one secret. Anyone with it can add readings.
#define INGEST_TOKEN ""
