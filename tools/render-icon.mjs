// Export the home-screen icon (launcher/sce_sys/icon0.svg) to the PNG format required by the console.
import { readFileSync, writeFileSync } from 'node:fs';
import { Resvg } from '@resvg/resvg-js';
const root = new URL('../', import.meta.url);
const svg = readFileSync(new URL('launcher/sce_sys/icon0.svg', root), 'utf8');
const png = new Resvg(svg, { fitTo: { mode: 'width', value: 512 }, font: { loadSystemFonts: false } })
  .render()
  .asPng();
writeFileSync(new URL('launcher/sce_sys/icon0.png', root), png);
console.log('Exported 512 × 512 Atmosphere launcher icon');
