import { useEffect, useMemo, useState } from "react";
import { Icon, Modal } from "./components";
import { Artwork } from "./Artwork";
import {
  bytes,
  type Game,
  type Job,
  type Storage,
  type LibraryTarget,
} from "./types";
const inactive = (job: Job) => ["complete", "cancelled"].includes(job.status);
const waiting = (job: Job) =>
  ["queued", "paused", "retrying"].includes(job.status);
export function Downloads({
  jobs,
  games,
  drives,
  action,
  clearHistory,
  browse,
  targetId,
  openLibrary,
}: {
  jobs: Job[];
  games: Game[];
  drives: Storage[];
  action: (id: string, name: string, remove?: boolean) => Promise<void>;
  clearHistory: (status: "complete" | "cancelled") => Promise<void>;
  browse: () => void;
  targetId: string | null;
  openLibrary: (target: Omit<LibraryTarget, "request">) => void;
}) {
  const [view, setView] = useState<
    "Active" | "Finished" | "Failed" | "Cancelled"
  >("Active");
  const [error, setError] = useState(""),
    [busy, setBusy] = useState("");
  const [cancel, setCancel] = useState<Job | null>(null),
    [clear, setClear] = useState(false);
  const [message, setMessage] = useState("");
  const targetStatus = jobs.find((j) => j.id === targetId)?.status;
  useEffect(() => {
    if (targetId && targetStatus)
      setView(
        targetStatus === "complete"
          ? "Finished"
          : targetStatus === "cancelled"
            ? "Cancelled"
            : targetStatus === "error"
              ? "Failed"
              : "Active",
      );
  }, [targetId, targetStatus]);
  useEffect(() => {
    if (!targetId) return;
    const row = [...document.querySelectorAll<HTMLElement>("[data-job]")].find(
      (el) => el.dataset.job === targetId,
    );
    row?.scrollIntoView({ block: "center" });
    row
      ?.querySelector<HTMLButtonElement>("button")
      ?.focus({ preventScroll: true });
  }, [targetId, view]);
  const gameByRelease = useMemo(
    () =>
      new Map(games.flatMap((g) => g.releases.map((r) => [r.id, g] as const))),
    [games],
  );
  const groups = {
    Active: jobs.filter((j) => !inactive(j) && j.status !== "error"),
    Finished: jobs.filter((j) => j.status === "complete"),
    Failed: jobs.filter((j) => j.status === "error"),
    Cancelled: jobs.filter((j) => j.status === "cancelled"),
  };
  const queued = jobs.filter(waiting);
  function restoreFocus() {
    requestAnimationFrame(() => {
      if (document.activeElement === document.body)
        document
          .querySelector<HTMLButtonElement>(
            ".download-tabs [aria-pressed=true]",
          )
          ?.focus();
    });
  }
  async function run(job: Job, name: string, remove = false) {
    setBusy(job.id);
    setError("");
    setMessage("");
    try {
      await action(job.id, name, remove);
      setCancel(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy("");
      restoreFocus();
    }
  }
  async function clearSelectedHistory() {
    setBusy("history");
    setError("");
    try {
      await clearHistory(view === "Cancelled" ? "cancelled" : "complete");
      setClear(false);
      setMessage(
        view === "Cancelled"
          ? "Cancelled history cleared. Kept partial files and disconnected downloads stay listed so you can recover them."
          : "Finished history cleared. Your downloaded files stay on the drive.",
      );
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy("");
      restoreFocus();
    }
  }
  return (
    <section className="page downloads">
      <div className="page-heading">
        <h1>Downloads</h1>
        <p>
          {groups.Active.length} active · {groups.Failed.length} need attention
        </p>
      </div>
      <div className="download-toolbar">
        <div className="download-tabs" role="group" aria-label="Download views">
          {(["Active", "Finished", "Failed", "Cancelled"] as const).map((v) => (
            <button
              key={v}
              aria-pressed={view === v}
              onClick={() => {
                setView(v);
                setMessage("");
              }}
            >
              {v} <span>{groups[v].length}</span>
            </button>
          ))}
        </div>
        {(view === "Finished" || view === "Cancelled") &&
          groups[view].length > 0 && (
            <button disabled={!!busy} onClick={() => setClear(true)}>
              Clear {view.toLowerCase()} history
            </button>
          )}
      </div>
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {message && (
        <p className="notice" role="status">
          {message}
        </p>
      )}
      {!groups[view].length ? (
        <div className="empty compact">
          <Icon name={view === "Finished" ? "check" : "download"} />
          <h2>
            {view === "Active"
              ? "Nothing in progress"
              : view === "Failed"
                ? "No failed downloads"
                : view === "Cancelled"
                  ? "No cancelled downloads"
                  : "No finished downloads"}
          </h2>
          <p>
            {view === "Active"
              ? "Choose a game to add it to your queue."
              : view === "Failed"
                ? "Downloads that need your attention appear here."
                : view === "Cancelled"
                  ? "Cancelled downloads appear here. Kept partial files can be resumed."
                  : "Successfully completed downloads appear here."}
          </p>
          <button className="primary" onClick={browse}>
            Browse games
          </button>
        </div>
      ) : (
        <div className="queue">
          {groups[view].map((j) => {
            const g = gameByRelease.get(j.releaseId),
              r = g?.releases.find((r) => r.id === j.releaseId);
            const title = g?.title || j.title || j.filename;
            const active = [
              "queued",
              "downloading",
              "retrying",
              "verifying",
            ].includes(j.status);
            const progress =
              j.total > 0
                ? Math.min(100, Math.max(0, (100 * j.received) / j.total))
                : 0;
            const position = queued.findIndex((q) => q.id === j.id);
            const seconds = Math.max(
              0,
              Math.ceil(j.retryAt - Date.now() / 1000),
            );
            const drive = drives.find((d) => d.id === j.storageId);
            return (
              <article
                className="queue-row"
                key={j.id}
                data-job={j.id}
                aria-label={title}
              >
                <Artwork
                  key={g?.cover || "/atmosphere.svg"}
                  src={g?.cover || "/atmosphere.svg"}
                  fallbackSrc={g?.coverFallback}
                  className={g ? undefined : "queue-placeholder"}
                  alt=""
                  decoding="async"
                  referrerPolicy="no-referrer"
                />
                <div className="queue-info">
                  <div className="queue-title">
                    <h2>{title}</h2>
                    <span className={`status ${j.status}`}>
                      {j.status === "complete" ? (
                        <>
                          <Icon name="check" /> Complete
                        </>
                      ) : j.status === "error" ? (
                        "Needs attention"
                      ) : (
                        j.status
                      )}
                    </span>
                  </div>
                  <p>
                    {r && `${r.provider} · ${r.format} · `}
                    {drive?.label || j.storageId}
                    {!drive && j.status !== "complete"
                      ? " · Reconnect this drive"
                      : ""}
                  </p>
                  <progress
                    max="100"
                    value={progress}
                    aria-label={`${title} progress`}
                  />
                  <div className="transfer-meta">
                    <span>
                      {bytes(j.received)} / {bytes(j.total)} ·{" "}
                      {progress.toFixed(1)}%
                    </span>
                    <span>
                      {j.status === "retrying"
                        ? seconds > 0
                          ? `Retrying in ${seconds}s`
                          : "Waiting to retry…"
                        : j.speed > 0
                          ? `${bytes(j.speed)}/s · ${Math.ceil((j.total - j.received) / j.speed / 60)} min remaining`
                          : j.status === "complete"
                            ? j.verification === "sha256"
                              ? "SHA-256 verified"
                              : "Size verified"
                            : `${bytes(Math.max(0, j.total - j.received))} remaining`}
                    </span>
                  </div>
                  {j.error && <p className="job-error">{j.error}</p>}
                  {j.status === "cancelled" && j.received > 0 && (
                    <p className="fine">
                      Partial file kept. Resume it, or delete the partial file
                      before removing this entry.
                    </p>
                  )}
                  <div className="queue-actions">
                    {j.status === "complete" && (
                      <button
                        className="primary"
                        onClick={() =>
                          openLibrary({
                            titleId: j.titleId,
                            path: j.path,
                            title,
                          })
                        }
                      >
                        Find in Library
                      </button>
                    )}
                    {j.status !== "complete" ? (
                      <>
                        <button
                          disabled={!!busy || (!active && !drive)}
                          onClick={() =>
                            void run(
                              j,
                              active
                                ? "pause"
                                : j.status === "error"
                                  ? "retry"
                                  : "resume",
                            )
                          }
                        >
                          {active
                            ? "Pause"
                            : j.status === "error"
                              ? "Retry download"
                              : j.status === "cancelled" && !j.received
                                ? "Start again"
                                : "Resume"}
                        </button>
                        <button disabled={!!busy} onClick={() => setCancel(j)}>
                          {j.status === "cancelled"
                            ? "Partial file options"
                            : "Cancel"}
                        </button>
                      </>
                    ) : null}
                    {waiting(j) && (
                      <div
                        className="queue-order"
                        role="group"
                        aria-label={`Queue position for ${title}`}
                      >
                        <button
                          disabled={!!busy || position <= 0}
                          aria-label={`Move ${title} up`}
                          onClick={() => void run(j, "move-up")}
                        >
                          ↑ Move up
                        </button>
                        <button
                          disabled={!!busy || position === queued.length - 1}
                          aria-label={`Move ${title} down`}
                          onClick={() => void run(j, "move-down")}
                        >
                          ↓ Move down
                        </button>
                      </div>
                    )}
                    {inactive(j) &&
                      (j.status === "complete" || j.received === 0) && (
                        <button
                          disabled={!!busy}
                          onClick={() => void run(j, "remove")}
                        >
                          Remove from history
                        </button>
                      )}
                  </div>
                </div>
              </article>
            );
          })}
        </div>
      )}
      {cancel && (
        <Modal title="Cancel download" close={() => !busy && setCancel(null)}>
          <h2>
            {cancel.status === "cancelled"
              ? "Manage the partial file"
              : "Cancel this download?"}
          </h2>
          <p>
            Keep the partial file to resume later, or delete only the partial
            file. Downloaded files are never deleted through history cleanup.
          </p>
          <div className="dialog-actions">
            <button
              onClick={() => void run(cancel, "cancel")}
              disabled={!!busy}
            >
              Keep partial file
            </button>
            <button
              className="danger"
              onClick={() => void run(cancel, "cancel", true)}
              disabled={!!busy}
            >
              Delete partial file
            </button>
            <button disabled={!!busy} onClick={() => setCancel(null)}>
              Go back
            </button>
          </div>
        </Modal>
      )}
      {clear && (
        <Modal
          title={`Clear ${view.toLowerCase()} history`}
          close={() => !busy && setClear(false)}
        >
          <h2>Clear {view.toLowerCase()} history?</h2>
          <p>
            {view === "Cancelled"
              ? "Remove cancelled entries with no partial file. Kept partials stay listed for recovery."
              : "Remove completed entries from history. Your downloaded files stay on the drive."}
          </p>
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={!!busy}
              onClick={() => void clearSelectedHistory()}
            >
              Clear history
            </button>
            <button disabled={!!busy} onClick={() => setClear(false)}>
              Keep history
            </button>
          </div>
        </Modal>
      )}
    </section>
  );
}
