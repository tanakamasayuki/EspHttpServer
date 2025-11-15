// en: Replace heading text after DOM is ready.
// ja: DOM 読み込み後に見出しテキストを更新します。
document.addEventListener('DOMContentLoaded', () => {
  const heading = document.querySelector('h1');
  if (heading) {
    heading.textContent = 'StaticFS Example (Loaded via LittleFS)';
  }
});
