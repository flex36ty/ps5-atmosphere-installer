// Stages a public release: the payload, the source archive of the exact commit it was built
// from (GPL-3.0-or-later), upstream sources of the copyleft components, licences and checksums.
// Nothing is uploaded.
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { checkRelease } from './release-check.mjs';
const root = new URL('../', import.meta.url);
const path = (relative) => fileURLToPath(new URL(relative, root));
const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');
const git = (...args) => execFileSync('git', args, { cwd: path('.'), encoding: 'utf8' }).trim();
const status = JSON.parse(readFileSync(path('release-status.json'), 'utf8'));
const blockers = checkRelease(status);
const catalogue = JSON.parse(readFileSync(path('catalog/catalog.json'), 'utf8'));
const gameCount = new Set(catalogue.map(r => r.gameId)).size;
const review = process.argv.includes('--review');
if (process.argv.slice(2).some(arg => arg !== '--review')) throw new Error('Only --review is supported.');
if (blockers.length && !review) throw new Error(`Cannot package a public release: ${blockers.join(', ')}`);
if (git('status', '--porcelain')) throw new Error('Commit or stash every change first: the source archive must match the payload.');
const commit = git('rev-parse', 'HEAD');
const artifact = path('build/atmosphere.elf');
const bytes = readFileSync(artifact);
if (bytes.subarray(0, 4).toString('hex') !== '7f454c46') throw new Error('Invalid ELF artifact');
if (statSync(artifact).mtimeMs < Number(git('log', '-1', '--format=%ct')) * 1000)
  throw new Error('build/atmosphere.elf is older than the last commit. Run npm run build and make payload first.');
const upstream = [
  { name: 'ps5-payload-sdk-d9c9519.tar.gz', url: 'https://github.com/ps5-payload-dev/sdk/archive/d9c9519116944a7f1c22d262012c53b80b3520b7.tar.gz', sha256: '44b541e77eef201afb4fbb6b32af9b97a4cdf893fc2d944d228ff46a8235fd10' },
  { name: 'libmicrohttpd-1.0.1.tar.gz', url: 'https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz', sha256: 'a89e09fc9b4de34dde19f4fcb4faaa1ce10299b9908db1132bbfa1de47882b94' },
];
const out = path(`build/${review ? 'review' : 'release'}-${status.version}/`);
rmSync(out, { recursive: true, force: true });
mkdirSync(`${out}licenses`, { recursive: true });
copyFileSync(artifact, `${out}atmosphere.elf`);
writeFileSync(`${out}atmosphere.elf.sha256`, `${sha256(bytes)}  atmosphere.elf\n`);
const source = `atmosphere-${status.version}-source.tar.gz`;
git('archive', '--format=tar.gz', `--prefix=atmosphere-${status.version}/`, '-o', `${out}${source}`, commit);
mkdirSync(path('build/upstream'), { recursive: true });
for (const dep of upstream) {
  const cached = path(`build/upstream/${dep.name}`);
  if (!existsSync(cached)) {
    const response = await fetch(dep.url);
    if (!response.ok) throw new Error(`Could not download ${dep.url}: HTTP ${response.status}`);
    writeFileSync(cached, Buffer.from(await response.arrayBuffer()));
  }
  if (sha256(readFileSync(cached)) !== dep.sha256) throw new Error(`Checksum mismatch: ${dep.name}`);
  copyFileSync(cached, `${out}${dep.name}`);
}
copyFileSync(path('LICENSE'), `${out}LICENSE`);
copyFileSync(path('THIRD-PARTY-NOTICES.md'), `${out}THIRD-PARTY-NOTICES.md`);
for (const file of readdirSync(path('licenses'))) copyFileSync(path(`licenses/${file}`), `${out}licenses/${file}`);
writeFileSync(`${out}RELEASE-NOTES.md`, `# Atmosphere ${status.version}

${review ? `UNRELEASED REVIEW BUNDLE. Outstanding gates: ${blockers.join(', ') || 'none'}. Do not upload this folder as a public release.\n\n` : ''}Browse on your PS5, download straight to your chosen storage, and manage your queue from the TV or your phone. Atmosphere is currently in **beta**.

## What's new

- **Your choice of payload manager.** Atmosphere adds or updates copies in Payload Manager and Homebrew Launcher only after you opt in from App settings. Existing copies are preserved, and your choices are remembered. Auto-start is separate; Atmosphere leaves the manager's global Autoload switch unchanged.
- **Your games, together in Library.** See installed games and sources on your drives, with clear availability and mount status. Catalogue pages show what you already have, and completed downloads can be opened in Library.
- **Manage games across drives.** With a compatible ShadowMount API, scan for games, mount or unmount supported sources, and copy or move games between drives. Review the destination and confirm before moving; follow progress from Library.
- **Understand your storage.** View drive capacity and measure game sizes when needed, with unfinished downloads accounted for before starting a copy or move.

## Download your way

- **One file per game.** Start with ${gameCount} games to browse, each available as a single download. No archive parts to collect.
- **Find your next download.** Explore recent releases or search for a title, then check its size, version and available download options before you choose.
- **Stay in control.** Download to your PS5's internal storage or an attached drive. Pause, resume or retry from the same queue, even after restarting Atmosphere.
- **Use the screen that suits you.** Browse with your controller, or pair your phone or computer to manage downloads on the same network. Missed the pairing code? Select **Show code on PS5** to see it again.
- **Update from the app.** Check for Atmosphere updates in **App settings → Update / reinstall**. Atmosphere tells you when a restart is needed and keeps your settings and queue saved.

## Install

Download \`atmosphere.elf\`, verify its SHA-256 below, then run it once through your payload manager or ELF loader. Open **Atmosphere** from the PS5 Media tab. On another device, visit \`http://<ps5-ip>:34177/\` and select **Pair devices**.

### Updating an existing installation

For beta.2 or earlier, replace the existing ELF in Payload Manager, pause downloads and stop only the identifiable Atmosphere process in **Active Processes**, then run the new ELF. If you cannot identify the process confidently, restart the console when convenient and launch the new ELF after the jailbreak. Keep the Atmosphere icon and saved data. Loading another ELF while Atmosphere is active saves it for the next start; it does not replace the running session.

From beta.3, use **App settings → Update / reinstall**, then **Stop Atmosphere to restart** and launch \`/data/atmosphere/atmosphere.elf\` or a synced manager copy. If you keep a manually imported copy outside opt-in sync, replace it yourself. Pairing, source choices and the download queue remain saved.

SHA-256: \`${sha256(bytes)}\`

## Before you download

This beta offers single-file **FFPFSC** downloads from **Archive.org**. Vikingfile is selectable but has no compatible download options yet. Atmosphere saves files and manages supported Library sources through ShadowMount. It does not extract archives, directly install packages, launch games, or download in rest mode. A confirmed ShadowMount scan may register or mount discovered games. Keep your PS5 awake while downloading.

Local tests cover opt-in persistence, untouched global manager settings, updates, transfers and Library operations against disposable fixtures. Library requires ShadowMount with the compatible v1 local API; supported actions depend on its capabilities. Hardware acceptance for Library and storage actions, full console downloads, online catalogue refresh, reboot/auto-start and other firmware versions remains pending. The reported etaHEN toggle interaction remains under investigation; this release does not claim to fix it. Links were checked for availability; their contents have not been fully downloaded and tested on console. Third-party URLs can change or disappear. Only download material you are entitled to obtain and use.

## Source and licences

Atmosphere is free software under the GNU GPL, version 3 or later. \`${source}\` is the complete source of this payload, built from commit \`${commit}\`. Third-party components and their sources are listed in THIRD-PARTY-NOTICES.md.
`);
const files = [
  'atmosphere.elf', 'atmosphere.elf.sha256', source, ...upstream.map((dep) => dep.name), 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'RELEASE-NOTES.md',
  ...readdirSync(`${out}licenses`).map((file) => `licenses/${file}`),
];
writeFileSync(`${out}SHA256SUMS`, files.map((file) => `${sha256(readFileSync(`${out}${file}`))}  ${file}\n`).join(''));
console.log(`Staged ${out} from commit ${commit}. ${review ? 'Review only; public-release gates remain enforced.' : 'Upload every file in it to the release.'} Nothing was uploaded.`);
