import { useEffect, useRef, useState } from "react";
import { Icon } from "./components";
import { Artwork } from "./Artwork";
import { Select } from "./Select";
import {
  bytes,
  type Game,
  type Job,
  type Storage,
  type System,
  type LibraryGame,
  type LibraryTarget,
} from "./types";
import { releaseState } from "./collection";
export function Details({
  game,
  drives,
  system,
  jobs,
  pair,
  download,
  openQueue,
  favorite,
  setFavorite,
  library,
  openLibrary,
}: {
  game: Game;
  drives: Storage[];
  system: System | null;
  jobs: Job[];
  pair: () => void;
  download: (id: string, storage: string) => Promise<void>;
  openQueue: (id?: string) => void;
  favorite: boolean;
  setFavorite: (value: boolean) => Promise<void>;
  library: LibraryGame[];
  openLibrary: (target: Omit<LibraryTarget, "request">) => void;
}) {
  const preferred =
    drives.find((d) => d.id === system?.preferredStorage) ||
    drives.find((d) => d.external) ||
    drives[0];
  const [destination, setDestination] = useState(preferred?.id || ""),
    [releaseId, setReleaseId] = useState(game.releases[0].id),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const [downloadAgain, setDownloadAgain] = useState(false);
  const page = useRef<HTMLElement>(null);
  // A source can be switched off by another paired device while this page is open.
  const release =
      game.releases.find((r) => r.id === releaseId) || game.releases[0],
    drive = drives.find((d) => d.id === (destination || preferred?.id)),
    // Jobs are per option and drive. A finished one can be downloaded again once its file is gone.
    existing = jobs.find(
      (job) => job.releaseId === release.id && job.storageId === drive?.id,
    ),
    planned = existing && !["complete", "cancelled"].includes(existing.status),
    afterDownload = drive
      ? drive.projectedFreeBytes -
        (planned
          ? 0
          : existing?.status === "cancelled"
            ? Math.max(0, existing.total - existing.received)
            : release.sizeBytes)
      : 0,
    enough = drive && afterDownload >= 16 * 1024 ** 2;
  const collection = releaseState(release, jobs, library);
  const libraryGame = collection?.libraryGame;
  const showDownload =
    downloadAgain || !collection || collection.kind === "related";
  useEffect(() => {
    setReleaseId(release.id);
    setError("");
    setDownloadAgain(false);
  }, [release.id]);
  useEffect(() => {
    window.scrollTo(0, 0);
    page.current
      ?.querySelector<HTMLElement>("button:not([disabled]), select")
      ?.focus({ preventScroll: true });
  }, [game.id]);
  async function start() {
    setPending(true);
    setError("");
    try {
      if (!drive) return;
      await download(release.id, drive.id);
      openQueue();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  return (
    <section ref={page} className="product" aria-labelledby="product-title">
      <div className="product-heading">
        <Artwork
          key={game.cover}
          className="product-cover"
          src={game.cover}
          fallbackSrc={game.coverFallback}
          alt=""
          priority
          decoding="async"
          referrerPolicy="no-referrer"
        />
        <div>
          <h1 id="product-title">{game.title}</h1>
          <p className="badges">
            <span className="platform">PS5</span>
            {game.genre && <span>{game.genre}</span>}
          </p>
          <button
            className="favorite-button"
            aria-pressed={favorite}
            disabled={pending}
            onClick={() => {
              if (!system?.paired) {
                pair();
                return;
              }
              setPending(true);
              setError("");
              void setFavorite(!favorite)
                .catch((e: Error) => setError(e.message))
                .finally(() => setPending(false));
            }}
          >
            {favorite ? "★ Saved to favourites" : "☆ Add to favourites"}
          </button>
        </div>
      </div>
      <fieldset className="release-picker" disabled={pending}>
        <legend>Download options</legend>
        <p className="release-intro">
          {game.releases.length > 1
            ? "Choose a source and format for this game."
            : "Available from your enabled sources."}
        </p>
        <div className="release-options">
          {game.releases.map((option) => (
            <button
              key={option.id}
              type="button"
              className="release-option"
              aria-pressed={release.id === option.id}
              aria-label={`${option.provider}, ${option.format}${option.version ? `, version ${option.version}` : ""}, ${option.titleId}, ${bytes(option.sizeBytes)}`}
              disabled={pending}
              onClick={() => setReleaseId(option.id)}
            >
              <span className="release-check" aria-hidden="true">
                {release.id === option.id && <Icon name="check" />}
              </span>
              <span className="release-label">
                <strong>{option.provider}</strong>
                <span>
                  {option.titleId}
                  {option.version && ` · v${option.version}`}
                </span>
              </span>
              <span className="release-specs">
                <span className="release-format">{option.format}</span>
                <span>{bytes(option.sizeBytes)}</span>
                {releaseState(option, jobs, library) && (
                  <span className="release-presence">
                    {releaseState(option, jobs, library)!.label}
                  </span>
                )}
              </span>
            </button>
          ))}
        </div>
      </fieldset>
      {!system?.paired ? (
        <button className="primary" onClick={pair}>
          Pair with your PS5
        </button>
      ) : (
        <>
          <div className="product-actions">
            {!downloadAgain && collection?.job ? (
              <button
                className="primary"
                onClick={() => openQueue(collection.job!.id)}
              >
                View download
              </button>
            ) : !downloadAgain &&
              collection?.kind === "library" &&
              libraryGame ? (
              <button
                className="primary"
                onClick={() =>
                  openLibrary({
                    titleId: libraryGame.titleId,
                    path: libraryGame.path,
                    title: libraryGame.title,
                  })
                }
              >
                View in Library
              </button>
            ) : (
              <button
                className="primary"
                onClick={() => {
                  void start();
                }}
                disabled={
                  pending ||
                  !enough ||
                  !(
                    system.platform === "ps5" ||
                    (system.platform === "fixture" &&
                      release.id === "ppsa04029-ffpfsc")
                  )
                }
              >
                <Icon name="download" />
                {pending
                  ? "Adding to queue…"
                  : existing || downloadAgain
                    ? "Download again"
                    : "Download to PS5"}
              </button>
            )}
            {showDownload && (
              <Select
                label="Save to"
                value={drive?.id || ""}
                disabled={pending}
                onChange={setDestination}
                options={[
                  { value: "", label: "Select storage", disabled: true },
                  ...drives.map((d) => ({
                    value: d.id,
                    label: `${d.label}, ${bytes(Math.max(0, d.projectedFreeBytes))} after queue`,
                  })),
                ]}
              />
            )}
          </div>
          {libraryGame && (
            <p className="collection-presence">
              {collection?.kind === "related"
                ? "A related copy is in Library. Its format, filename or version does not confirm a match for this option."
                : `In your Library on ${libraryGame.location}.`}
              {collection?.kind === "related" && (
                <button
                  onClick={() =>
                    openLibrary({
                      titleId: libraryGame.titleId,
                      path: libraryGame.path,
                      title: libraryGame.title,
                    })
                  }
                >
                  View related copy
                </button>
              )}
            </p>
          )}
          {!downloadAgain &&
            (collection?.kind === "library" ||
              collection?.job?.status === "complete") && (
              <button
                className="download-another"
                onClick={() => setDownloadAgain(true)}
              >
                Download another copy
              </button>
            )}
          {collection?.job?.status === "complete" && (
            <p className="fine">
              Download history records a completed transfer. Find it in Library
              to check its current location.
            </p>
          )}
          {showDownload && drive && (
            <p className="destination">Saves to {drive.path}</p>
          )}
          {showDownload && drive && (
            <dl
              className="storage-budget"
              aria-label="Download storage estimate"
            >
              <div>
                <dt>Free now</dt>
                <dd>{bytes(drive.freeBytes)}</dd>
              </div>
              <div>
                <dt>Unfinished downloads</dt>
                <dd>{bytes(drive.pendingBytes)}</dd>
              </div>
              <div className={afterDownload < 0 ? "budget-short" : ""}>
                <dt>
                  {planned ? "After your queue" : "After queue + this download"}
                </dt>
                <dd>
                  {afterDownload < 0
                    ? `${bytes(-afterDownload)} short`
                    : bytes(afterDownload)}
                </dd>
              </div>
            </dl>
          )}
          {showDownload &&
            system.platform !== "desktop" &&
            (!drive ? (
              <p className="notice">Connect a writable drive to your PS5.</p>
            ) : (
              !enough && (
                <p className="notice">
                  Not enough space after unfinished downloads. Free space or
                  cancel an item in Downloads.
                </p>
              )
            ))}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {game.description && <p className="description">{game.description}</p>}
      <dl className="facts">
        {game.publisher && (
          <div>
            <dt>Publisher</dt>
            <dd>{game.publisher}</dd>
          </div>
        )}
        {game.releaseDate && (
          <div>
            <dt>PS5 release</dt>
            <dd>
              {new Intl.DateTimeFormat("en", {
                dateStyle: "medium",
                timeZone: "UTC",
              }).format(new Date(game.releaseDate))}
            </dd>
          </div>
        )}
        <div>
          <dt>Format</dt>
          <dd>{release.format}</dd>
        </div>
        <div>
          <dt>Title ID</dt>
          <dd>{release.titleId}</dd>
        </div>
        <div>
          <dt>Source</dt>
          <dd>{release.provider}</dd>
        </div>
        {release.version && (
          <div>
            <dt>Version</dt>
            <dd>{release.version}</dd>
          </div>
        )}
        <div>
          <dt>Download size</dt>
          <dd>{bytes(release.sizeBytes)}</dd>
        </div>
      </dl>
      <p className="fine">
        Saves a file to your console. Installation and launching are separate.
      </p>
    </section>
  );
}
