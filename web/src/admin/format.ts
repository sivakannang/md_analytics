/** 1536 -> "1.5 KB", 5_300_000 -> "5.1 MB". */
export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit] ?? 'GB'}`;
}

/** "just now", "5 min ago", "3 h ago", "2 days ago"; "—" for none. */
export function ago(iso: string | null | undefined, now = Date.now()): string {
  if (!iso) return '—';
  const seconds = Math.max(0, Math.round((now - new Date(iso).getTime()) / 1000));
  if (seconds < 60) return 'just now';
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
  if (seconds < 86_400) return `${Math.floor(seconds / 3600)} h ago`;
  const days = Math.floor(seconds / 86_400);
  return `${days} day${days === 1 ? '' : 's'} ago`;
}

/** 22 Oct, 14:05 (IST). */
export function istTime(iso: string | null | undefined): string {
  if (!iso) return '—';
  return new Date(iso).toLocaleString('en-IN', {
    timeZone: 'Asia/Kolkata',
    day: 'numeric',
    month: 'short',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  });
}

/** Short device description from a User-Agent string. */
export function device(userAgent: string): string {
  if (!userAgent) return 'Unknown device';
  const browser = /Edg\//.test(userAgent)
    ? 'Edge'
    : /Chrome\//.test(userAgent)
      ? 'Chrome'
      : /Firefox\//.test(userAgent)
        ? 'Firefox'
        : /Safari\//.test(userAgent)
          ? 'Safari'
          : userAgent.split(/[ /]/)[0] || 'Unknown';
  const os = /iPhone|iPad/.test(userAgent)
    ? 'iOS'
    : /Android/.test(userAgent)
      ? 'Android'
      : /Mac OS X/.test(userAgent)
        ? 'macOS'
        : /Windows/.test(userAgent)
          ? 'Windows'
          : /Linux/.test(userAgent)
            ? 'Linux'
            : '';
  return os ? `${browser} on ${os}` : browser;
}

const EVENT_LABEL: Record<string, string> = {
  sign_in: 'Signed in',
  sign_in_failed: 'Failed sign-in',
  sign_out: 'Signed out',
  password_changed: 'Changed password',
  session_expired: 'Session expired',
  session_revoked: 'Signed out by admin',
  account_deleted: 'Deleted account',
};

export function eventLabel(type: string): string {
  return EVENT_LABEL[type] ?? type;
}
