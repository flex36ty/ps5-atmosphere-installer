import { useEffect, useState } from "react";
import { Icon } from "./components";
import { Select } from "./Select";
import { bytes, type LibraryGame } from "./types";
import type { LibraryModel } from "./useLibrary";

export function LibraryFeedback({
  model,
  titleId,
}: {
  model: LibraryModel;
  titleId?: string;
}) {
  const action = model.snapshot?.action;
  if (!action || (titleId && action.titleId !== titleId)) return null;
  return (
    <p
      className={
        action.state === "error"
          ? "form-error library-feedback"
          : "notice library-feedback"
      }
      role={action.state === "error" ? "alert" : "status"}
    >
      {action.message}
    </p>
  );
}

export function StorageProgress({ model }: { model: LibraryModel }) {
  const [error, setError] = useState("");
  const job = model.snapshot?.storageJob?.id ? model.snapshot.storageJob : null;
  if (!job?.id && !model.snapshot?.storageBusy) return null;
  const progress =
    job?.state === "completed"
      ? 100
      : job?.totalBytes
        ? Math.min(100, (100 * job.processedBytes) / job.totalBytes)
        : 0;
  const title =
    model.snapshot?.games.find((g) => g.titleId === job?.titleId)?.title ||
    job?.titleId ||
    "Storage operation";
  return (
    <section className="storage-job" aria-label="Storage operation progress">
      <div className="storage-job-heading">
        <div>
          <p className="library-eyebrow">STORAGE ACTIVITY</p>
          <h3>{title}</h3>
        </div>
        <span className="library-label">
          {job
            ? {
                idle: "Idle",
                preparing: "Preparing",
                measuring: "Measuring",
                transferring: "Transferring",
                finalizing: "Finishing",
                completed: "Complete",
                failed: "Failed",
                cancelled: "Cancelled",
              }[job.state] || job.state
            : "Checking status"}
        </span>
      </div>
      {job && (
        <>
          <p>
            {job.operation === "copy"
              ? "Copy"
              : job.operation === "move"
                ? "Move"
                : job.operation}{" "}
            ·{" "}
            {job.active && !job.totalBytes
              ? "Measuring…"
              : `${bytes(job.processedBytes)} / ${bytes(job.totalBytes)}`}
          </p>
          <progress
            max={100}
            value={progress}
            aria-label="Storage operation progress"
          />
          <div className="transfer-meta">
            <span>{progress.toFixed(1)}%</span>
            <span>
              {job.active && job.speed > 0
                ? `${bytes(job.speed)}/s`
                : job.active && job.cancelRequested
                  ? "Cancellation requested"
                  : ""}
            </span>
          </div>
          <p className="library-path">{job.source}</p>
          {job.destination && (
            <p className="library-path">To {job.destination}</p>
          )}
          {job.error && (
            <p className="form-error" role="alert">
              {job.error}
            </p>
          )}
          {job.active && (
            <button
              disabled={
                model.actionBusy ||
                !job.cancellable ||
                job.cancelRequested ||
                !model.can("storage_job_cancel")
              }
              onClick={() => {
                setError("");
                void model
                  .run("cancel", { jobId: job.id })
                  .catch((e) => setError((e as Error).message));
              }}
            >
              {job.cancelRequested
                ? "Cancelling…"
                : job.cancellable
                  ? "Cancel operation"
                  : "Finishing, cannot cancel"}
            </button>
          )}
        </>
      )}
      {model.snapshot?.storageError && (
        <p className="form-error" role="alert">
          {model.snapshot.storageError}
        </p>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}

export function GameActions({
  game,
  model,
}: {
  game: LibraryGame;
  model: LibraryModel;
}) {
  const [mode, setMode] = useState<"copy" | "move" | "unmount" | null>(null);
  const [destination, setDestination] = useState("");
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  useEffect(() => {
    setMode(null);
    setError("");
    setDestination("");
  }, [game.sourceKey]);
  const targets = (model.storage?.destinations || []).filter(
    (d) =>
      !d.readOnly &&
      d.path !== game.path &&
      !d.path.startsWith(`${game.path}/`) &&
      d.path !== game.path.slice(0, game.path.lastIndexOf("/")),
  );
  const target = targets.find((d) => d.id === destination);
  const storageFresh =
    !!model.storage?.updatedAt && !model.storage.stale && !model.storageError;
  const disabled = model.working || model.stale || sending || !game.onDrive;
  const transfer = game.managed && game.canManageSource && game.onDrive;
  const sharedTitles =
    model.snapshot?.games.filter((g) => g.path === game.path).length || 1;
  async function run(action: string) {
    setSending(true);
    setError("");
    try {
      await model.run(action, {
        titleId: game.titleId,
        sourceKey: game.sourceKey,
        ...(target ? { destinationId: target.id } : {}),
        confirmed: true,
      });
      setMode(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  return (
    <div className="library-game-actions">
      <LibraryFeedback model={model} titleId={game.titleId} />
      {model.snapshot?.storageJob?.titleId === game.titleId && (
        <StorageProgress model={model} />
      )}
      <div className="dialog-actions">
        {game.managed &&
          model.can(game.mounted ? "unmount_game" : "mount_game") && (
            <button
              className="primary"
              disabled={disabled}
              onClick={() =>
                game.mounted ? setMode("unmount") : void run("mount")
              }
            >
              {game.mounted ? "Unmount game" : "Mount game"}
            </button>
          )}
        {transfer &&
          model.can("copy_game_source") &&
          model.can("storage_job_status") && (
            <button
              disabled={disabled || game.mounted}
              onClick={() => {
                setMode("copy");
                void model.readStorage(true);
              }}
            >
              Copy to drive
            </button>
          )}
        {transfer &&
          model.can("move_game_source") &&
          model.can("storage_job_status") && (
            <button
              disabled={disabled || game.mounted}
              onClick={() => {
                setMode("move");
                void model.readStorage(true);
              }}
            >
              Move to drive
            </button>
          )}
      </div>
      {transfer && game.mounted && (
        <p className="fine">
          Unmount this game before copying or moving its source.
        </p>
      )}
      {!game.managed && (
        <p className="fine">
          This item supports viewing details only. ShadowMount does not manage
          its source.
        </p>
      )}
      {mode && (
        <section
          className="library-action-confirm"
          aria-label={`${mode} game confirmation`}
        >
          <h3>
            {mode === "unmount"
              ? "Unmount this game?"
              : `${mode === "copy" ? "Copy" : "Move"} ${game.title}`}
          </h3>
          <p>
            {mode === "unmount"
              ? "Close the game on your PS5 before unmounting its source."
              : mode === "move"
                ? "Move the source to another location. The original is removed only after the move succeeds."
                : "Create a second copy on the selected drive. The original stays in place."}
          </p>
          {mode !== "unmount" && (
            <>
              {sharedTitles > 1 && (
                <p className="notice">
                  This source is shared by {sharedTitles} Library titles. This
                  operation applies to the whole source.
                </p>
              )}
              <p className="library-path">From {game.path}</p>
              <Select
                label="Destination drive"
                value={target?.id || ""}
                onChange={setDestination}
                options={[
                  { value: "", label: "Choose a destination", disabled: true },
                  ...targets.map((d) => ({
                    value: d.id,
                    label: `${d.label} · ${d.path} · ${bytes(d.freeBytes)} free`,
                  })),
                ]}
                disabled={disabled || !storageFresh}
              />
              {target && (
                <p className="library-path">
                  To {target.path}/{game.path.split("/").pop()}
                </p>
              )}
              {!storageFresh && (
                <p className="notice">
                  {model.storage?.error ||
                    model.storageError ||
                    "Checking available destinations…"}
                </p>
              )}
              {storageFresh && !targets.length && (
                <p className="notice">
                  Connect another writable drive, or configure another scan
                  location in ShadowMount.
                </p>
              )}
              <p className="fine">
                Atmosphere checks the source and free space before starting. Pause
                active and queued downloads first. Existing destination files
                are not overwritten.
              </p>
            </>
          )}
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={
                disabled || (mode !== "unmount" && (!target || !storageFresh))
              }
              onClick={() => void run(mode)}
            >
              {sending
                ? "Starting…"
                : mode === "unmount"
                  ? "Confirm unmount"
                  : mode === "copy"
                    ? "Start copy"
                    : "Confirm move"}
            </button>
            <button disabled={sending} onClick={() => setMode(null)}>
              Back
            </button>
          </div>
        </section>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </div>
  );
}

export function StorageOverview({
  model,
  openGame,
  feedback = false,
}: {
  model: LibraryModel;
  openGame: (g: LibraryGame) => void;
  feedback?: boolean;
}) {
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");
  const drives = model.storage?.drives || [];
  const drive = drives.find((d) => d.id === selected) || drives[0];
  const games = (model.snapshot?.games || [])
    .filter(
      (g) =>
        g.onDrive &&
        drive &&
        ((drive.mountPoint === "/user" && g.path.startsWith("/data/")) ||
          g.path === drive.mountPoint ||
          g.path.startsWith(`${drive.mountPoint}/`)),
    )
    .sort(
      (a, b) =>
        (b.sizeBytes ?? -1) - (a.sizeBytes ?? -1) ||
        a.title.localeCompare(b.title),
    );
  const sizesKnown = games.filter((g) => g.sizeBytes !== null).length;
  return (
    <section className="storage-overview" aria-label="Game storage">
      <div className="storage-overview-heading">
        <div>
          <h2>Your drives</h2>
          <p>See available space and manage where your games live.</p>
        </div>
        <button
          disabled={!!model.storage?.busy}
          onClick={() => void model.readStorage(true)}
        >
          {model.storage?.busy ? "Refreshing…" : "Refresh drives"}
        </button>
      </div>
      {(model.storageError || model.storage?.error) && (
        <p className="notice" role="status">
          {model.storageError || model.storage?.error}
        </p>
      )}
      {model.storage?.stale && !!model.storage.updatedAt && (
        <p className="notice">
          Saved drive information. Reconnect and refresh before managing files.
        </p>
      )}
      {!drives.length ? (
        <div className="empty compact">
          <Icon name="usb" />
          <h3>
            {model.storage?.busy ? "Reading drives…" : "No drives reported"}
          </h3>
          <p>Drive management uses ShadowMount’s storage API.</p>
        </div>
      ) : (
        <>
          <div className="drive-grid" role="group" aria-label="Select drive">
            {drives.map((d) => (
              <button
                key={d.id}
                className="drive-card"
                aria-pressed={drive?.id === d.id}
                onClick={() => setSelected(d.id)}
              >
                <span className="drive-card-title">
                  <Icon name="usb" />
                  <strong>{d.label}</strong>
                </span>
                <span>
                  {d.path}
                  {d.readOnly ? " · Read only" : ""}
                </span>
                <span className="storage-meter">
                  <span
                    style={{
                      width: `${d.totalBytes ? Math.min(100, (100 * d.usedBytes) / d.totalBytes) : 0}%`,
                    }}
                  />
                </span>
                <strong>{bytes(d.freeBytes)} free</strong>
                <span>
                  {bytes(d.usedBytes)} used of {bytes(d.totalBytes)}
                </span>
              </button>
            ))}
          </div>
          <div className="storage-overview-heading">
            <div>
              <h3>Games on {drive?.label}</h3>
              <p>
                {games.length} games · {sizesKnown} sizes measured
              </p>
            </div>
            <button
              disabled={model.working || !model.can("list_games")}
              onClick={() => {
                setError("");
                void model
                  .run("measure")
                  .catch((e) => setError((e as Error).message));
              }}
            >
              {model.snapshot?.action?.action === "measure" && model.actionBusy
                ? "Measuring…"
                : "Measure game sizes"}
            </button>
          </div>
          <p className="fine">
            Sizes are measured on request. Shared source files can appear under
            multiple titles; drive usage includes other files.
          </p>
          <div className="storage-game-list">
            {games.map((g) => (
              <button
                className="storage-game"
                key={g.titleId}
                onClick={() => openGame(g)}
              >
                <span>
                  <strong>{g.title}</strong>
                  <small>
                    {g.format} ·{" "}
                    {g.mounted
                      ? "Mounted"
                      : g.installed
                        ? "Installed"
                        : "On drive"}
                  </small>
                </span>
                <span>
                  {g.sizeBytes !== null
                    ? bytes(g.sizeBytes)
                    : g.sizeStatus === "unavailable"
                      ? "Size unavailable"
                      : "Not measured"}
                </span>
                <span aria-hidden="true">›</span>
              </button>
            ))}
          </div>
          {!games.length && (
            <p className="notice">
              No Library games currently match this drive.
            </p>
          )}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {feedback && <LibraryFeedback model={model} />}
      <StorageProgress model={model} />
    </section>
  );
}
