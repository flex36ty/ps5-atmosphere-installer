export interface Release {
  id: string;
  gameId: string;
  titleId: string;
  title: string;
  genre: string | null;
  tagline: string | null;
  description: string | null;
  sizeBytes: number;
  provider: string;
  sourceId: SourceId;
  format: "FFPFSC" | "exFAT";
  version: string | null;
  filename: string;
  cover: string;
  hero: string;
  coverFallback?: string | null;
  heroFallback?: string | null;
  artworkLayout: "wide" | "ambient";
  publisher: string | null;
  releaseDate: string | null;
  addedAt?: string | null;
  consoleVerified: boolean;
}
export interface Game {
  id: string;
  title: string;
  genre: string | null;
  tagline: string | null;
  description: string | null;
  cover: string;
  hero: string;
  coverFallback?: string | null;
  heroFallback?: string | null;
  artworkLayout: "wide" | "ambient";
  publisher: string | null;
  releaseDate: string | null;
  addedAt: string | null;
  releases: Release[];
}
export type SourceId = "vikingfile" | "archive";
export interface SourceSettings {
  enabled: SourceId[];
  acknowledged: boolean;
  noticeVersion: number;
  options: {
    id: SourceId;
    label: string;
    releaseCount: number;
    downloadReady: boolean;
  }[];
}
export interface Storage {
  id: string;
  label: string;
  path: string;
  freeBytes: number;
  totalBytes: number;
  pendingBytes: number;
  projectedFreeBytes: number;
  external: boolean;
}
export interface Job {
  titleId: string;
  gameId: string;
  format: string;
  path: string;
  title: string;
  order: number;
  id: string;
  releaseId: string;
  storageId: string;
  filename: string;
  status:
    | "queued"
    | "downloading"
    | "paused"
    | "retrying"
    | "verifying"
    | "complete"
    | "cancelled"
    | "error";
  received: number;
  total: number;
  speed: number;
  retryAt: number;
  error: string;
  verification: string;
}
export interface LibraryGame {
  titleId: string;
  title: string;
  platform: "ps5" | "ps4" | "unknown";
  format: string;
  path: string;
  runtimePath: string;
  sourceKey: string;
  location: string;
  installed: boolean;
  mounted: boolean;
  onDrive: boolean;
  managed: boolean;
  canManageSource: boolean;
  installedSizeBytes: number | null;
  sizeBytes: number | null;
  sizeStatus: "ready" | "unknown" | "unavailable";
  cover?: string;
  coverFallback?: string | null;
}
export interface LibraryAction {
  id: string;
  action: string;
  titleId: string;
  state: "queued" | "running" | "complete" | "error";
  message: string;
}
export interface StorageJob {
  id: number;
  operation: string;
  state: string;
  titleId: string;
  source: string;
  destination: string;
  error: string;
  result: number;
  totalBytes: number;
  processedBytes: number;
  speed: number;
  active: boolean;
  cancellable: boolean;
  cancelRequested: boolean;
}
export interface LibrarySnapshot {
  status: "idle" | "ready" | "unavailable" | "unsupported" | "error";
  message: string;
  provider: string;
  providerVersion: string;
  busy: boolean;
  stale: boolean;
  checkedAt: number;
  updatedAt: number;
  refreshAfter: number;
  games: LibraryGame[];
  capabilities: string[];
  action: LibraryAction | null;
  storageJob: StorageJob | null;
  storageError: string;
  storageBusy: boolean;
}
export interface LibraryDrive {
  id: string;
  path: string;
  mountPoint: string;
  label: string;
  filesystem: string;
  readOnly: boolean;
  totalBytes: number;
  freeBytes: number;
  usedBytes: number;
}
export interface LibraryStorage {
  drives: LibraryDrive[];
  destinations: LibraryDrive[];
  busy: boolean;
  stale: boolean;
  updatedAt: number;
  error: string;
}
export interface LibraryTarget {
  titleId?: string;
  path?: string;
  title: string;
  request: number;
}
export interface System {
  catalogueRevision: number;
  name: string;
  version: string;
  platform: "desktop" | "ps5" | "fixture";
  paired: boolean;
  stateHealthy: boolean;
  preferredStorage: string;
  consoleValidated: boolean;
  targetFirmware: string;
  localSessionAvailable: boolean;
  pairNotificationAvailable: boolean;
  httpPort: number;
  launcherStatus: "checking" | "ready" | "error" | "not-included";
}
export interface AutoStart {
  available: boolean;
  savedPath?: string;
  saved?: boolean;
  preferencesError?: string;
  payloadManager?: {
    installed: boolean;
    listed: boolean;
    managed: boolean;
    enabled: boolean;
    switchOn: boolean;
    otherEntries: number;
  };
  autoloadTxt?: { files: { path: string; enabled: boolean }[] };
  homebrewLauncher?: { installed: boolean; listed: boolean; managed: boolean };
  etaHEN?: { installed: boolean };
}
export const bytes = (n: number) =>
  n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : `${(n / 1e6).toFixed(1)} MB`;
