// Web page settings. Both values are public by design: the publishable key
// can only read readings (Row Level Security), it cannot change anything.
// Find them in Supabase: Project Settings > API Keys.
export const SUPABASE_URL = 'https://obdltdjykzrxxpozupqt.supabase.co';
export const SUPABASE_KEY = ''; // paste the publishable key here: 'sb_publishable_...'

// Time zone used for "today" and daily totals (must match the SQL view).
export const TIME_ZONE = 'Europe/Dublin';

// A device counts as offline when it has not sent anything for this long.
export const OFFLINE_AFTER_MIN = 150;
