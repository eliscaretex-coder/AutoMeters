// Web page settings. Both values are public by design: the publishable key
// can only read readings (Row Level Security), it cannot change anything.
// Find them in Supabase: Project Settings > API Keys.
export const SUPABASE_URL = 'https://obdltdjykzrxxpozupqt.supabase.co';
export const SUPABASE_KEY = 'sb_publishable_BQwFl3_6XV9igdkYUJTFbA_UHM5OhOZ';

// Time zone used for "today" and daily totals (must match the SQL view).
export const TIME_ZONE = 'Europe/Dublin';

// A device counts as offline when it has not sent anything for this long.
export const OFFLINE_AFTER_MIN = 150;
