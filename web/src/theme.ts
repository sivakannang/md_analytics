// Colour theme: dark by default on every device, light on request, remembered per browser.
// index.html applies the saved choice before first paint; this switches it at runtime.

const KEY = 'mda.theme';

export type Theme = 'dark' | 'light';

export function currentTheme(): Theme {
  return document.documentElement.dataset.theme === 'light' ? 'light' : 'dark';
}

export function setTheme(theme: Theme): void {
  document.documentElement.dataset.theme = theme;
  try {
    localStorage.setItem(KEY, theme);
  } catch {
    // Private mode or blocked storage: the choice lasts until reload.
  }
}
