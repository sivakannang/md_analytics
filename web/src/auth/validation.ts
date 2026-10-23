// Client-side checks mirroring the server's rules (ADR-0014), for instant feedback.
// The server still validates everything — these only save a round trip.

export const USERNAME_HINT = '3–32 characters: letters, digits, _ . -';
export const PASSWORD_HINT = 'At least 8 characters. A short phrase works well.';
export const EMAIL_HINT = 'For alerts and signals. Not shared with anyone.';
export const PHONE_HINT = 'Indian mobile number, e.g. 98765 43210';

export function usernameError(username: string): string | null {
  if (!/^[A-Za-z0-9_.-]{3,32}$/.test(username)) {
    return `Username must be ${USERNAME_HINT}`;
  }
  return null;
}

export function passwordError(password: string): string | null {
  const length = [...password].length; // code points, like the server
  if (length < 8) {
    return 'Password must be at least 8 characters';
  }
  if (length > 128) {
    return 'Password must be at most 128 characters';
  }
  return null;
}

export function emailError(email: string): string | null {
  // Same shape check as the server; the verification email will be the real test.
  if (!/^[^\s@]{1,64}@[^\s@]+\.[^\s@]{2,}$/.test(email.trim()) || email.length > 254) {
    return 'Enter a valid email address';
  }
  return null;
}

export function phoneError(phone: string): string | null {
  let digits = phone.replace(/[\s\-()]/g, '');
  if (digits.startsWith('+91')) {
    digits = digits.slice(3);
  } else if (digits.length === 12 && digits.startsWith('91')) {
    digits = digits.slice(2);
  } else if (digits.length === 11 && digits.startsWith('0')) {
    digits = digits.slice(1);
  }
  if (!/^[6-9]\d{9}$/.test(digits)) {
    return 'Enter a valid Indian mobile number (10 digits starting with 6–9)';
  }
  return null;
}
