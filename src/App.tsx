import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { useAtmosphere } from "./useAtmosphere";
import { useController } from "./controller";
import { Backdrop, GameTile, Icon, Modal, StorageRows } from "./components";
import { Details } from "./Details";
import { Downloads } from "./Downloads";
import { Library } from "./Library";
import { useLibrary } from "./useLibrary";
import { StorageOverview } from "./LibraryActions";
import { gameState } from "./collection";
import { Select } from "./Select";
import { AutoStart } from "./AutoStart";
import { Loader } from "./Loader";
import { Sources } from "./Sources";
import { SMB } from "./SMB";
import { Pairing } from "./Pairing";
import type { Game, LibraryTarget } from "./types";
import { browseGames, defaultFilters, type BrowseFilters } from "./browse";
export default function App() {
  const atmosphere = useAtmosphere();
  const [tab, setTab] = useState<
      "Discover" | "Browse" | "SMB" | "Library" | "Downloads"
    >("SMB"),
    [gameId, setGameId] = useState<string | null>(null),
    [focusedId, setFocusedId] = useState("");
  const [storageOpen, setStorageOpen] = useState(false),
    [sourcesOpen, setSourcesOpen] = useState(false),
    [autoOpen, setAutoOpen] = useState(false),
    [pairOpen, setPairOpen] = useState(false);
  const [libraryTarget, setLibraryTarget] = useState<LibraryTarget | null>(
    null,
  );
  const [downloadTarget, setDownloadTarget] = useState<string | null>(null);
  const library = useLibrary(
    !!atmosphere.system?.paired,
    tab === "Library" || storageOpen,
  );
  const collectionStates = useMemo(
    () =>
      new Map(
        atmosphere.games.map((g) => [
          g.id,
          gameState(
            g,
            atmosphere.jobs,
            library.stale ? [] : library.snapshot?.games || [],
          ),
        ]),
      ),
    [atmosphere.games, atmosphere.jobs, library.snapshot?.games, library.stale],
  );
  const [filters, setFilters] = useState<BrowseFilters>(defaultFilters);
  const [filtersOpen, setFiltersOpen] = useState(false);
  const query = filters.query;
  const [pageIndex, setPageIndex] = useState(0);
  const game = atmosphere.games.find((entry) => entry.id === gameId);
  const search = useRef<HTMLInputElement>(null),
    returnTo = useRef("");
  const favorites = useMemo(() => new Set(atmosphere.favorites), [atmosphere.favorites]);
  function filter<K extends keyof BrowseFilters>(
    key: K,
    value: BrowseFilters[K],
  ) {
    setFilters((current) => ({ ...current, [key]: value }));
    setPageIndex(0);
  }
  function resetFilters() {
    setFilters(defaultFilters);
    setPageIndex(0);
  }
  const back = useCallback(() => {
    if (pairOpen) setPairOpen(false);
    else if (sourcesOpen) setSourcesOpen(false);
    else if (storageOpen) setStorageOpen(false);
    else if (autoOpen) setAutoOpen(false);
    else if (game) setGameId(null);
    else setTab("SMB");
  }, [game, storageOpen, autoOpen, pairOpen, sourcesOpen]);
  useController(back);
  const filtered = useMemo(
    () => browseGames(atmosphere.games, filters, favorites),
    [atmosphere.games, filters, favorites],
  );
  const pageCount = Math.max(1, Math.ceil(filtered.length / 48));
  const currentPage = Math.min(pageIndex, pageCount - 1);
  const visibleGames = filtered.slice(currentPage * 48, (currentPage + 1) * 48);
  const latestGames = useMemo(
    () =>
      atmosphere.games
        .filter(
          (g) =>
            g.releaseDate &&
            g.releaseDate <= new Date().toISOString().slice(0, 10),
        )
        .sort(
          (a, b) =>
            b.releaseDate!.localeCompare(a.releaseDate!) ||
            a.title.localeCompare(b.title),
        )
        .slice(0, 20),
    [atmosphere.games],
  );
  const active = atmosphere.jobs.filter((j) =>
    ["queued", "downloading", "retrying", "verifying"].includes(j.status),
  ).length;
  // Start with the same game the first rail focuses. Avoid fetching a different
  // full-size background only to replace it as soon as initial focus is applied.
  const focused =
    atmosphere.games.find((g) => g.id === focusedId) ||
    latestGames[0] ||
    atmosphere.games[0];
  const firstCovers = window.innerWidth < 700 ? 3 : 6;
  const discover = tab === "Discover" && !game;
  useEffect(() => {
    if (atmosphere.games.length && !document.querySelector('[role="dialog"]'))
      document
        .querySelector<HTMLElement>(".collection .game-tile")
        ?.focus({ preventScroll: true });
  }, [atmosphere.games.length]);
  useEffect(() => {
    if (gameId && !game) setGameId(null);
  }, [gameId, game]);
  useEffect(() => {
    // Returning from a game page puts focus back on the tile that opened it.
    if (game || !returnTo.current) return;
    document
      .querySelector<HTMLElement>(returnTo.current)
      ?.focus({ preventScroll: true });
    returnTo.current = "";
  }, [game]);
  function open(g: Game) {
    const row = document.activeElement
      ?.closest("[data-row]")
      ?.getAttribute("data-row");
    returnTo.current = `${row ? `[data-row="${row}"] ` : ""}[data-game="${g.id}"]`;
    setGameId(g.id);
  }
  function changePage(next: number) {
    setPageIndex(next);
    requestAnimationFrame(() => {
      const tile = document.querySelector<HTMLElement>(
        ".browse-grid .game-tile",
      );
      tile?.focus({ preventScroll: true });
      document
        .querySelector(".page-heading")
        ?.scrollIntoView({ block: "start" });
    });
  }
  function go(t: typeof tab) {
    setGameId(null);
    setTab(t === "Discover" || t === "Browse" ? "SMB" : t);
    setDownloadTarget(null);
    setLibraryTarget(null);
  }
  function openLibrary(target: Omit<LibraryTarget, "request">) {
    go("Library");
    setStorageOpen(false);
    setLibraryTarget({ ...target, request: Date.now() });
  }
  function openDownload(id?: string) {
    go("Downloads");
    setDownloadTarget(id || null);
  }
  function showSearch() {
    go("SMB");
    requestAnimationFrame(() => document.querySelector<HTMLInputElement>('.smb-page input[type="search"]')?.focus());
  }
  function paired(action: () => void) {
    return () => (atmosphere.system?.paired ? action() : setPairOpen(true));
  }
  return (
    <div className={`app ${discover ? "discover" : ""}`}>
      <Backdrop
        games={atmosphere.games}
        active={game?.id || (discover && focused?.id) || ""}
      />
      <header className="header">
        {game && (
          <button
            className="top-icon"
            data-label="Back"
            aria-label="Back"
            onClick={back}
          >
            <Icon name="back" />
          </button>
        )}
        <button
          className="brand"
          onClick={() => go("SMB")}
          aria-label="Atmosphere home"
        >
          <img src="/atmosphere.svg" alt="" />
          Atmosphere
          <span className="beta-label">Beta</span>
        </button>
        {/* A game page is its own screen: no tabs or tools, Circle or Back returns. */}
        {!game && (
          <>
            <nav aria-label="Main navigation">
              {(["SMB", "Library", "Downloads"] as const).map(
                (t) => (
                  <button
                    key={t === "SMB" ? "SERVERS" : t}
                    className={tab === t ? "active" : ""}
                    aria-current={tab === t ? "page" : undefined}
                    onClick={() => go(t)}
                  >
                    {t === "SMB" ? "SERVERS" : t}
                    {t === "Downloads" && active > 0 && (
                      <span className="queue-count">{active}</span>
                    )}
                  </button>
                ),
              )}
            </nav>
            <div className="utilities">
              <button
                className="pair-devices"
                onClick={() => setPairOpen(true)}
              >
                Pair devices
              </button>
              <button
                className="top-icon"
                data-label="Search"
                aria-label="Search games"
                onClick={showSearch}
              >
                <Icon name="search" />
              </button>
              <button
                className="top-icon"
                data-label="USB storage"
                aria-label="USB storage"
                onClick={paired(() => setStorageOpen(true))}
              >
                <Icon name="usb" />
              </button>
              <button
                className="top-icon"
                data-label="App settings"
                aria-label="App settings"
                onClick={paired(() => setAutoOpen(true))}
              >
                <Icon name="settings" />
              </button>
            </div>
          </>
        )}
      </header>
      {atmosphere.error && (
        <div className="connection-error" role="status">
          {atmosphere.error}
          <button onClick={() => location.reload()}>Reconnect</button>
        </div>
      )}
      {atmosphere.system && !atmosphere.system.stateHealthy && (
        <div className="connection-error" role="alert">
          Atmosphere cannot save its queue. Check free space on the console’s
          internal storage.
        </div>
      )}
      <main>
        {tab !== "Downloads" &&
        tab !== "SMB" &&
        tab !== "Library" &&
        atmosphere.sources &&
        !atmosphere.sources.acknowledged ? (
          <section className="page source-setup">
            <Sources
              settings={atmosphere.sources}
              paired={!!atmosphere.system?.paired}
              pair={() => setPairOpen(true)}
              save={atmosphere.saveSources}
            />
          </section>
        ) : game ? (
          <Details
            key={game.id}
            game={game}
            drives={atmosphere.storage}
            system={atmosphere.system}
            jobs={atmosphere.jobs}
            pair={() => setPairOpen(true)}
            download={atmosphere.download}
            openQueue={openDownload}
            library={library.stale ? [] : library.snapshot?.games || []}
            openLibrary={openLibrary}
            favorite={favorites.has(game.id)}
            setFavorite={(value) => atmosphere.favorite(game.id, value)}
          />
        ) : tab === "Discover" ? (
          focused ? (
            <>
              <section className="hero" aria-live="polite">
                <h1>{focused.title}</h1>
                <p className="badges">
                  <span className="platform">PS5</span>
                  {focused.genre && <span>{focused.genre}</span>}
                </p>
                {focused.tagline && (
                  <p className="tagline">{focused.tagline}</p>
                )}
                <button className="primary" onClick={() => open(focused)}>
                  View game
                </button>
              </section>
              {[
                {
                  id: "latest",
                  title: "Latest releases",
                  games: latestGames,
                  note: "Newest first",
                },
                {
                  id: "all",
                  title: "All games",
                  games: atmosphere.games,
                  note: `${atmosphere.games.length} games`,
                },
              ].map((row) => (
                <section
                  key={row.id}
                  className="collection"
                  data-row={row.id}
                  aria-labelledby={`${row.id}-title`}
                >
                  <div className="section-heading">
                    <h2 id={`${row.id}-title`}>{row.title}</h2>
                    <span>{row.note}</span>
                  </div>
                  {row.games.length ? (
                    <div className="game-rail">
                      {row.games.map((g, index) => (
                        <GameTile
                          key={g.id}
                          game={g}
                          status={collectionStates.get(g.id)?.label}
                          favorite={favorites.has(g.id)}
                          showDate={row.id === "latest"}
                          priority={
                            g.id === focused.id ||
                            ((row.id === "latest" || !latestGames.length) &&
                              index < firstCovers)
                          }
                          open={() => open(g)}
                          onFocus={() => setFocusedId(g.id)}
                        />
                      ))}
                    </div>
                  ) : (
                    <p className="muted">
                      Release dates aren’t available for these games yet.
                    </p>
                  )}
                </section>
              ))}
            </>
          ) : (
            <div className="empty">
              {atmosphere.sources?.acknowledged ? (
                <Icon name="sources" />
              ) : (
                <Loader />
              )}
              <h1>
                {atmosphere.error
                  ? "Atmosphere isn’t reachable"
                  : atmosphere.sources?.acknowledged
                    ? atmosphere.sources.enabled.length
                      ? "No games from your selected sources"
                      : "No sources enabled"
                    : "Opening Atmosphere…"}
              </h1>
              {atmosphere.error ? (
                <p>Check that Atmosphere is running on your PS5, then reconnect.</p>
              ) : (
                atmosphere.sources?.acknowledged && (
                  <>
                    <p>
                      {atmosphere.sources.enabled.length
                        ? "There are no compatible releases from these sources in this build. Choose another source to browse."
                        : "Choose a download source to see its games."}
                    </p>
                    <button
                      className="primary"
                      onClick={() => setSourcesOpen(true)}
                    >
                      Choose sources
                    </button>
                  </>
                )
              )}
            </div>
          )
        ) : tab === "Browse" ? (
          <section className="page browse">
            <div className="page-heading">
              <h1>Browse</h1>
              <span>
                {filtered.length} {filtered.length === 1 ? "game" : "games"}
              </span>
            </div>
            <div className="filters">
              <label className="search-field">
                <Icon name="search" />
                <input
                  ref={search}
                  type="search"
                  placeholder="Search games or title ID"
                  aria-label="Search games or title ID"
                  value={query}
                  onChange={(e) => {
                    filter("query", e.target.value);
                  }}
                />
              </label>
            </div>
            <div className="browse-controls">
              <div
                className="collection-tabs"
                role="group"
                aria-label="Browse collection"
              >
                <button
                  aria-pressed={filters.collection === "all"}
                  onClick={() => filter("collection", "all")}
                >
                  All games
                </button>
                <button
                  aria-pressed={filters.collection === "recent"}
                  onClick={() => filter("collection", "recent")}
                >
                  Recently added
                </button>
                <button
                  aria-pressed={filters.collection === "favorites"}
                  onClick={() =>
                    atmosphere.system?.paired
                      ? filter("collection", "favorites")
                      : setPairOpen(true)
                  }
                >
                  Favourites
                </button>
              </div>
              <button
                className="filter-toggle"
                aria-expanded={filtersOpen}
                aria-controls="browse-filters"
                onClick={() => setFiltersOpen((value) => !value)}
              >
                Filters and sort
                {filters.source ||
                filters.format ||
                filters.size ||
                filters.sort !== "title"
                  ? " · Applied"
                  : ""}{" "}
                {filtersOpen ? "▴" : "▾"}
              </button>
              <div
                id="browse-filters"
                className={`browse-selects ${filtersOpen ? "expanded" : ""}`}
              >
                <Select
                  label="Source"
                  value={filters.source}
                  onChange={(value) => filter("source", value)}
                  options={[
                    { value: "", label: "All sources" },
                    ...Array.from(
                      new Map(
                        atmosphere.games.flatMap((g) =>
                          g.releases.map(
                            (r) => [r.sourceId, r.provider] as const,
                          ),
                        ),
                      ),
                    ).map(([value, label]) => ({ value, label })),
                  ]}
                />
                <Select
                  label="Format"
                  value={filters.format}
                  onChange={(value) => filter("format", value)}
                  options={[
                    { value: "", label: "All formats" },
                    ...Array.from(
                      new Set(
                        atmosphere.games.flatMap((g) =>
                          g.releases.map((r) => r.format),
                        ),
                      ),
                    ).map((value) => ({ value, label: value })),
                  ]}
                />
                <Select
                  label="Download size"
                  value={filters.size}
                  onChange={(value) => filter("size", value)}
                  options={[
                    { value: "", label: "Any size" },
                    { value: "small", label: "Under 10 GB" },
                    { value: "medium", label: "10–50 GB" },
                    { value: "large", label: "50–100 GB" },
                    { value: "huge", label: "100 GB or more" },
                  ]}
                />
                <Select
                  label="Sort by"
                  value={filters.sort}
                  onChange={(value) => filter("sort", value)}
                  options={[
                    { value: "title", label: "Title A–Z" },
                    { value: "title-desc", label: "Title Z–A" },
                    { value: "release", label: "Latest release" },
                    { value: "added", label: "Recently added" },
                    { value: "size", label: "Smallest download" },
                    { value: "size-desc", label: "Largest download" },
                  ]}
                />
                <button className="reset-filters" onClick={resetFilters}>
                  Reset filters
                </button>
              </div>
              {filters.collection === "recent" && (
                <p className="fine">
                  Added to Atmosphere in the last 30 days, regardless of the game's
                  release date.
                </p>
              )}
              {(filters.sort === "size" || filters.sort === "size-desc") && (
                <p className="fine">
                  Sorted by the smallest download option that matches your
                  filters.
                </p>
              )}
            </div>
            {filtered.length ? (
              <>
                <div className="game-rail browse-grid" data-row="browse">
                  {visibleGames.map((g, index) => (
                    <GameTile
                      key={g.id}
                      game={g}
                      status={collectionStates.get(g.id)?.label}
                      favorite={favorites.has(g.id)}
                      priority={index < firstCovers}
                      open={() => open(g)}
                    />
                  ))}
                </div>
                {pageCount > 1 && (
                  <nav className="catalog-pages" aria-label="Catalogue pages">
                    <button
                      disabled={currentPage === 0}
                      onClick={() => changePage(currentPage - 1)}
                    >
                      Previous
                    </button>
                    <span aria-live="polite">
                      Page {currentPage + 1} of {pageCount}
                    </span>
                    <button
                      disabled={currentPage + 1 === pageCount}
                      onClick={() => changePage(currentPage + 1)}
                    >
                      Next
                    </button>
                  </nav>
                )}
              </>
            ) : (
              <div className="empty compact">
                <h2>No games found</h2>
                <p>
                  {atmosphere.games.length
                    ? filters.collection === "favorites"
                      ? "Save a game from its details page, or adjust your filters."
                      : "Try another title, title ID or filter."
                    : "Your selected sources have no games in this build."}
                </p>
                <button
                  onClick={() => {
                    if (!atmosphere.games.length) setSourcesOpen(true);
                    else {
                      resetFilters();
                    }
                  }}
                >
                  {atmosphere.games.length ? "Reset filters" : "Choose sources"}
                </button>
              </div>
            )}
          </section>
        ) : tab === "SMB" ? (
          <SMB paired={!!atmosphere.system?.paired} pair={() => setPairOpen(true)} drives={atmosphere.storage} />
        ) : tab === "Library" ? (
          <Library
            model={library}
            target={libraryTarget}
            paired={!!atmosphere.system?.paired}
            pair={() => setPairOpen(true)}
          />
        ) : !atmosphere.system?.paired ? (
          <section className="page">
            <h1>Downloads</h1>
            <div className="empty">
              <Icon name="download" />
              <h2>See your PS5’s downloads</h2>
              <p>Pair this device using the code shown on your PS5.</p>
              <button className="primary" onClick={() => setPairOpen(true)}>
                Pair with your PS5
              </button>
            </div>
          </section>
        ) : (
          <Downloads
            jobs={atmosphere.jobs}
            games={atmosphere.games}
            drives={atmosphere.storage}
            action={atmosphere.action}
            clearHistory={atmosphere.clearHistory}
            browse={() => go("SMB")}
            targetId={downloadTarget}
            openLibrary={openLibrary}
          />
        )}
      </main>
      <footer>
        {atmosphere.system?.platform === "fixture" && (
          <span>Local test fixture. No console connected.</span>
        )}
        <div className="controller-hints">
          <span>
            <b>×</b>Select
          </span>
          <button onClick={back}>
            <b>○</b>Back
          </button>
        </div>
      </footer>
      {storageOpen && (
        <Modal
          title="Storage"
          close={() => setStorageOpen(false)}
          className="storage-modal"
        >
          <h1>Storage</h1>
          <StorageOverview
            feedback
            model={library}
            openGame={(g) =>
              openLibrary({ titleId: g.titleId, path: g.path, title: g.title })
            }
          />
          <h2 className="download-storage-heading">Download destinations</h2>
          <p>Downloads save inside the selected drive’s homebrew folder.</p>
          <StorageRows drives={atmosphere.storage} />
          <div className="dialog-actions">
            <button
              onClick={() => {
                void atmosphere.refresh();
              }}
            >
              Refresh storage
            </button>
            <button
              onClick={() => {
                setStorageOpen(false);
                setPairOpen(true);
              }}
            >
              Pair another device
            </button>
          </div>
        </Modal>
      )}
      {autoOpen && (
        <AutoStart system={atmosphere.system} close={() => setAutoOpen(false)} />
      )}
      {sourcesOpen && atmosphere.sources && (
        <Modal
          title="Download sources"
          close={() => setSourcesOpen(false)}
          className="sources-modal"
        >
          <Sources
            settings={atmosphere.sources}
            paired={!!atmosphere.system?.paired}
            pair={() => {
              setSourcesOpen(false);
              setPairOpen(true);
            }}
            save={atmosphere.saveSources}
            saved={() => {
              setSourcesOpen(false);
              go("Discover");
            }}
          />
        </Modal>
      )}
      {pairOpen && (
        <Pairing
          system={atmosphere.system}
          pair={atmosphere.pair}
          close={() => setPairOpen(false)}
        />
      )}
    </div>
  );
}
