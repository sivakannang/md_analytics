// Sets the colour theme before first paint so the page doesn't flash (src/theme.ts). A file,
// not inline in index.html: the gateway's Content-Security-Policy allows only same-origin scripts.
try {
  document.documentElement.dataset.theme =
    localStorage.getItem('mda.theme') === 'light' ? 'light' : 'dark';
} catch {
  document.documentElement.dataset.theme = 'dark';
}
