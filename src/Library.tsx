import { useEffect, useMemo, useRef, useState } from "react";
import { Icon, Modal } from "./components";
import { bytes, type LibraryGame, type LibraryTarget } from "./types";
import type { LibraryModel } from "./useLibrary";
import {
  GameActions,
  LibraryFeedback,
  StorageOverview,
  StorageProgress,
} from "./LibraryActions";
import { Artwork } from "./Artwork";
import { Select } from "./Select";

const emptyGames: LibraryGame[] = [];
const views = [
  "All games",
  "Installed",
  "On drive",
  "Mounted",
  "Source missing",
] as const;
type View = (typeof views)[number];
const matches = (game: LibraryGame, view: View) =>
  view === "Installed"
    ? game.installed
    : view === "On drive"
      ? game.onDrive
      : view === "Mounted"
        ? game.mounted
        : view === "Source missing"
          ? !game.onDrive
          : true;
const platformName = (game: LibraryGame) =>
  game.platform === "unknown"
    ? "Platform unknown"
    : game.platform.toUpperCase();
const checkedTime = (value: number) =>
  new Date(value * 1000).toLocaleString([], {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });

export function Library({
  paired,
  pair,
  model,
  target,
}: {
  paired: boolean;
  pair: () => void;
  model: LibraryModel;
  target: LibraryTarget | null;
}) {
  return paired ? (
    <Inventory model={model} target={target} />
  ) : (
    <section className="page library">
      <h1>Library</h1>
      <div className="empty">
        <Icon name="sources" />
        <h2>See the games on your PS5</h2>
        <p>
          Pair this device to see installed games and games available on your
          drives.
        </p>
        <button className="primary" onClick={pair}>
          Pair with your PS5
        </button>
      </div>
    </section>
  );
}
function StatusLabels({ game }: { game: LibraryGame }) {
  return (
    <span className="library-labels">
      <span className={`library-label ${game.installed ? "installed" : ""}`}>
        {game.installed ? "Installed" : "Not installed"}
      </span>
      {game.mounted && <span className="library-label mounted">Mounted</span>}
      <span
        className={`library-label ${game.onDrive ? "available" : "missing"}`}
      >
        {game.onDrive ? "On drive" : "Source missing"}
      </span>
    </span>
  );
}
function Inventory({
  model,
  target,
}: {
  model: LibraryModel;
  target: LibraryTarget | null;
}) {
  const { snapshot, error, refreshAt } = model;
  const [section, setSection] = useState<"games" | "storage">("games");
  const [scan, setScan] = useState(false);
  const [scanError, setScanError] = useState("");
  const [lookup, setLookup] = useState<LibraryTarget | null>(null);
  const consumed = useRef(0);
  const [query, setQuery] = useState("");
  const [view, setView] = useState<View>("All games");
  const [location, setLocation] = useState("");
  const [format, setFormat] = useState("");
  const [page, setPage] = useState(0);
  const [selected, setSelected] = useState<string | null>(null);
  const games = snapshot?.games || emptyGames;
  const rows = useMemo(
    () =>
      games
        .filter(
          (g) =>
            matches(g, view) &&
            (!location || g.location === location) &&
            (!format || g.format === format) &&
            `${g.title} ${g.titleId} ${g.path} ${g.platform}`
              .toLowerCase()
              .includes(query.trim().toLowerCase()),
        )
        .sort(
          (a, b) =>
            a.title.localeCompare(b.title) ||
            a.titleId.localeCompare(b.titleId),
        ),
    [games, view, location, format, query],
  );
  const pages = Math.max(1, Math.ceil(rows.length / 48));
  const currentPage = Math.min(page, pages - 1);
  const current = games.find((g) => g.titleId === selected);
  const stale = model.stale;
  const busy = (!snapshot && !error) || (!error && !!snapshot?.busy);
  const ready = !!snapshot?.updatedAt;
  const count = (v: View) => games.filter((g) => matches(g, v)).length;
  useEffect(() => {
    if (!target || consumed.current === target.request) return;
    consumed.current = target.request;
    setSection("games");
    setView("All games");
    setLocation("");
    setFormat("");
    setPage(0);
    setQuery(target.titleId || "");
    setLookup(target);
    void model.read(true);
  }, [target, model.read]);
  useEffect(() => {
    if (!lookup) return;
    const found = games.find(
      (g) =>
        g.onDrive &&
        (lookup.path ? g.path === lookup.path : g.titleId === lookup.titleId),
    );
    if (found) {
      setSelected(found.titleId);
      setLookup(null);
    }
  }, [games, lookup]);
  useEffect(() => {
    if (selected && !current) setSelected(null);
  }, [selected, current]);
  useEffect(() => {
    // Do not leave a hidden filter active after a drive or format disappears.
    if (location && !games.some((g) => g.location === location)) {
      setLocation("");
      setPage(0);
    }
    if (format && !games.some((g) => g.format === format)) {
      setFormat("");
      setPage(0);
    }
  }, [games, location, format]);
  function reset() {
    setQuery("");
    setView("All games");
    setLocation("");
    setFormat("");
    setPage(0);
  }
  function changePage(next: number) {
    setPage(next);
    requestAnimationFrame(() =>
      document.querySelector<HTMLElement>(".library-card")?.focus(),
    );
  }
  return (
    <section className="page library">
      <div className="library-heading">
        <div>
          <p className="library-eyebrow">YOUR COLLECTION</p>
          <h1>Library</h1>
          <p className="library-intro">
            Installed on your PS5. Available on your drives. All in one place.
          </p>
        </div>
        <div className="library-heading-actions">
          <button
            onClick={() => void model.read(true)}
            disabled={busy || Date.now() < refreshAt}
          >
            {busy ? "Refreshing…" : "Refresh library"}
          </button>
          {model.can("rescan") && (
            <button
              disabled={model.working}
              onClick={() => {
                setScan(true);
                setScanError("");
              }}
            >
              Scan for games
            </button>
          )}
        </div>
      </div>
      <div className="library-sync" role="status">
        <span className={`library-dot ${ready && !stale ? "online" : ""}`} />
        <span>
          {ready
            ? `${stale ? "Saved snapshot" : "Last refreshed"} · ${checkedTime(snapshot.updatedAt)}`
            : busy
              ? "Reading your game library…"
              : "Library unavailable"}
        </span>
        {ready && (
          <span>
            From {snapshot.provider}
            {snapshot.providerVersion ? ` ${snapshot.providerVersion}` : ""}
          </span>
        )}
      </div>
      {(error || snapshot?.message) && (
        <div className="notice library-notice" role="status">
          <strong>{error || snapshot?.message}</strong>
          {ready && (
            <p>
              Showing the last successful result. Game and drive status may have
              changed.
            </p>
          )}
        </div>
      )}
      <LibraryFeedback model={model} />
      {lookup && (
        <div className="notice" role="status">
          <strong>Looking for {lookup.title}</strong>
          <p>
            The completed file is not in the current Library inventory. Check
            that its drive is connected, then scan for games. A completed
            download alone does not confirm installation.
          </p>
          <button onClick={() => setLookup(null)}>Dismiss</button>
        </div>
      )}
      {!ready ? (
        <div className="empty compact">
          <Icon name="sources" />
          <h2>{busy ? "Finding your games" : "Connect your library"}</h2>
          <p>
            {busy
              ? "Reading installed titles and drive availability from ShadowMount."
              : "Library uses ShadowMount’s local games API on your PS5. Start a version with that API enabled, then refresh here."}
          </p>
          {!busy && (
            <p className="fine">
              Games are not inferred from download history. An unavailable
              library does not mean your games are missing.
            </p>
          )}
        </div>
      ) : (
        <>
          <div
            className="collection-tabs library-sections"
            role="group"
            aria-label="Library section"
          >
            <button
              aria-pressed={section === "games"}
              onClick={() => setSection("games")}
            >
              Games
            </button>
            <button
              aria-pressed={section === "storage"}
              onClick={() => setSection("storage")}
            >
              Storage
            </button>
          </div>
          {section === "storage" ? (
            <StorageOverview
              model={model}
              openGame={(g) => setSelected(g.titleId)}
            />
          ) : (
            <>
              <StorageProgress model={model} />
              <div
                className="library-tabs"
                role="group"
                aria-label="Library views"
              >
                {views.map((v) => (
                  <button
                    key={v}
                    aria-pressed={view === v}
                    onClick={() => {
                      setView(v);
                      setPage(0);
                    }}
                  >
                    {v}
                    <span>{count(v)}</span>
                  </button>
                ))}
              </div>
              <div className="library-filters">
                <label className="search-field">
                  <Icon name="search" />
                  <input
                    aria-label="Search library"
                    value={query}
                    onChange={(e) => {
                      setQuery(e.target.value);
                      setPage(0);
                    }}
                    placeholder="Search games, title IDs or paths"
                  />
                </label>
                <Select
                  label="Location"
                  value={location}
                  onChange={(value) => {
                    setLocation(value);
                    setPage(0);
                  }}
                  options={[
                    { value: "", label: "All locations" },
                    ...[...new Set(games.map((g) => g.location))]
                      .sort()
                      .map((value) => ({ value, label: value })),
                  ]}
                />
                <Select
                  label="Format"
                  value={format}
                  onChange={(value) => {
                    setFormat(value);
                    setPage(0);
                  }}
                  options={[
                    { value: "", label: "All formats" },
                    ...[...new Set(games.map((g) => g.format))]
                      .sort()
                      .map((value) => ({ value, label: value })),
                  ]}
                />
              </div>
              <div className="library-results">
                <p>
                  {rows.length} {rows.length === 1 ? "game" : "games"}
                  {stale ? " in saved snapshot" : ""}
                </p>
                <p>Installed and on-drive counts can overlap.</p>
              </div>
              {rows.length ? (
                <div className="library-grid">
                  {rows
                    .slice(currentPage * 48, (currentPage + 1) * 48)
                    .map((g) => (
                      <button
                        key={g.titleId}
                        className="library-card"
                        onClick={() => setSelected(g.titleId)}
                        aria-label={`View library details for ${g.title}`}
                      >
                        <span className="library-cover">
                          <Artwork
                            key={g.cover || "mark"}
                            src={g.cover || "/atmosphere.svg"}
                            fallbackSrc={g.coverFallback}
                            alt=""
                            decoding="async"
                            referrerPolicy="no-referrer"
                          />
                        </span>
                        <span className="library-card-content">
                          <span className="library-title">{g.title}</span>
                          <StatusLabels game={g} />
                          <span className="library-location">
                            <Icon name="usb" />
                            {g.location}
                          </span>
                        </span>
                      </button>
                    ))}
                </div>
              ) : (
                <div className="empty compact">
                  <h2>
                    {games.length
                      ? "No matching games"
                      : "No games reported yet"}
                  </h2>
                  <p>
                    {games.length
                      ? "Try a different title, status, location or format."
                      : "ShadowMount has not reported any installed or on-drive games. Refresh after its inventory updates."}
                  </p>
                  {games.length > 0 && (
                    <button onClick={reset}>Reset library filters</button>
                  )}
                </div>
              )}
              {pages > 1 && (
                <nav className="catalog-pages" aria-label="Library pages">
                  <button
                    disabled={currentPage === 0}
                    onClick={() => changePage(currentPage - 1)}
                  >
                    Previous
                  </button>
                  <span aria-live="polite">
                    Page {currentPage + 1} of {pages}
                  </span>
                  <button
                    disabled={currentPage + 1 === pages}
                    onClick={() => changePage(currentPage + 1)}
                  >
                    Next
                  </button>
                </nav>
              )}
              <p className="library-footnote">
                Shows the titles and source locations reported by ShadowMount.
                Refresh reads its current inventory. Scan for games asks
                ShadowMount to discover sources and may register or mount them.
              </p>
            </>
          )}
        </>
      )}
      {scan && (
        <Modal
          title="Scan for games"
          close={() => !model.actionBusy && setScan(false)}
        >
          <h1>Scan your drives?</h1>
          <p>
            ShadowMount will search its configured locations for games. It may
            register games on the console or mount detected sources. If a game
            is running, the scan may wait until it is safe.
          </p>
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={model.working}
              onClick={() => {
                void model
                  .run("scan", { confirmed: true })
                  .then(() => setScan(false))
                  .catch((e) => setScanError((e as Error).message));
              }}
            >
              Start scan
            </button>
            <button disabled={model.actionBusy} onClick={() => setScan(false)}>
              Back
            </button>
          </div>
          {scanError && (
            <p className="form-error" role="alert">
              {scanError}
            </p>
          )}
        </Modal>
      )}
      {current && (
        <Modal
          title="Library game details"
          close={() => setSelected(null)}
          className="library-details"
        >
          <p className="library-eyebrow">
            {platformName(current)} · {current.titleId}
          </p>
          <h1>{current.title}</h1>
          <StatusLabels game={current} />
          {stale && (
            <p className="notice">
              Saved snapshot. Refresh the library before relying on these
              statuses.
            </p>
          )}
          <GameActions game={current} model={model} />
          <dl className="library-facts">
            <div>
              <dt>Installation</dt>
              <dd>
                {current.installed
                  ? "Registered on the console"
                  : "Not installed on the console"}
              </dd>
            </div>
            <div>
              <dt>Mount status</dt>
              <dd>{current.mounted ? "Mounted" : "Not mounted"}</dd>
            </div>
            <div>
              <dt>Source</dt>
              <dd>
                {current.onDrive
                  ? "Available on drive"
                  : "Unavailable; the drive or source may be missing"}
              </dd>
            </div>
            <div>
              <dt>Format</dt>
              <dd>{current.format}</dd>
            </div>
            <div>
              <dt>Location</dt>
              <dd>{current.location}</dd>
            </div>
            <div>
              <dt>Source path</dt>
              <dd className="library-path">{current.path || "Not reported"}</dd>
            </div>
            {current.runtimePath && current.runtimePath !== current.path && (
              <div>
                <dt>Game path</dt>
                <dd className="library-path">{current.runtimePath}</dd>
              </div>
            )}
            <div>
              <dt>Source size</dt>
              <dd>
                {current.sizeBytes !== null
                  ? bytes(current.sizeBytes)
                  : current.sizeStatus === "unavailable"
                    ? "Unavailable"
                    : "Use Measure game sizes in Storage"}
              </dd>
            </div>
            <div>
              <dt>Size reported by console</dt>
              <dd>
                {current.installedSizeBytes === null
                  ? "Not reported"
                  : bytes(current.installedSizeBytes)}
              </dd>
            </div>
          </dl>
          <p className="fine">
            Installed means registered on the console. Mounted means its source
            is currently mounted. On drive means the source is available. These
            statuses do not confirm that a game will launch.
          </p>
        </Modal>
      )}
    </section>
  );
}
