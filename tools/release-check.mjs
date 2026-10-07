import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
export function checkRelease(s) {
  if (s.channel === 'beta' && /^\d+\.\d+\.\d+(?:-beta\.\d+)?$/.test(s.version || '')) {
    const blockers = ['licenseCleared', 'localRegressionPassed', 'catalogLinksVerified', 'betaLimitationsDocumented'].filter(k => s[k] !== true);
    const deferral = s.betaConsoleCheckDeferred;
    const ownerDeferred = deferral?.version === s.version && deferral?.authorizedByOwner === true &&
      typeof deferral?.reason === 'string' && deferral.reason.trim().length > 0;
    if (s.betaConsoleSmokePassed !== true && !ownerDeferred) blockers.push('betaConsoleSmokePassed');
    return blockers;
  }
  return ['licenseCleared', 'consoleTestPassed', 'catalogSourcesVerifiedOnConsole', 'homeScreenLifecycleVerified'].filter(k => s[k] !== true);
}
if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const state = JSON.parse(readFileSync(new URL('../release-status.json', import.meta.url), 'utf8'));
  const blockers = checkRelease(state);
  if (blockers.length) { console.error(`Public payload release blocked: ${blockers.join(', ')}.\n${state.reason}`); process.exit(1); }
  console.log('Release evidence gates passed.');
}
