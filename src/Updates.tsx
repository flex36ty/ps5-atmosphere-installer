import { useEffect, useState } from "react";
import { api } from "./api";
import { bytes } from "./types";
interface Status {
  available: boolean;
  runningVersion: string;
  savedVersion: string;
  latestVersion: string;
  checksum: string;
  phase: string;
  busy: boolean;
  error: string;
  restartRequired: boolean;
  received: number;
  retryAt: number;
  checkedAt: number;
  checkAfter: number;
}
export function Updates({ onStopped }: { onStopped: () => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const [confirmStop, setConfirmStop] = useState(false);
  const [stopped, setStopped] = useState(false);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    if (stopped) return;
    let active = true;
    let loading = false;
    const refresh = async () => {
      if (loading) return;
      loading = true;
      try {
        const next = await api<Status>("/updates");
        if (active) setStatus(next);
      } catch (e) {
        if (active) setError((e as Error).message);
      } finally {
        loading = false;
      }
    };
    void refresh();
    const timer = setInterval(() => {
      setNow(Date.now());
      void refresh();
    }, 2000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [stopped]);
  async function act(action: string) {
    setSending(true);
    setError("");
    try {
      const next = await api<Status>("/updates", {
        action,
        ...(action === "install"
          ? { version: status?.latestVersion, checksum: status?.checksum }
          : action === "stop"
            ? { confirmed: true }
            : {}),
      });
      setStatus(next);
      if (action === "stop") {
        setStopped(true);
        onStopped();
      }
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const disabled = sending || status?.busy;
  const wait = Math.max(
    0,
    Math.ceil(((status?.retryAt || 0) * 1000 - now) / 1000),
  );
  const checkWait = Math.max(
    wait,
    Math.ceil(((status?.checkAfter || 0) * 1000 - now) / 1000),
  );
  return (
    <section className="updates-panel" aria-labelledby="updates-title">
      <h2 id="updates-title">Update / reinstall</h2>
      {stopped ? (
        <div className="notice" role="status">
          <strong>Atmosphere is stopping.</strong>
          <p>
            Open your payload manager and run{" "}
            <b>/data/atmosphere/atmosphere.elf</b> once, or use a copy you
            opted to keep in sync. Then reopen the Atmosphere icon or reconnect here.
            Resume your downloads when you’re ready.
          </p>
          <button onClick={() => location.reload()}>Reconnect to Atmosphere</button>
        </div>
      ) : !status ? (
        <p>Loading installed version…</p>
      ) : (
        <>
          <dl className="update-versions">
            <div>
              <dt>Running</dt>
              <dd>{status.runningVersion}</dd>
            </div>
            <div>
              <dt>Saved for next start</dt>
              <dd>{status.savedVersion || "Not recorded yet"}</dd>
            </div>
            {status.latestVersion && (
              <div>
                <dt>Latest release</dt>
                <dd>{status.latestVersion}</dd>
              </div>
            )}
          </dl>
          {!status.available ? (
            <p className="notice">
              Open Atmosphere on your PS5 or pair your phone with it to update the
              console’s saved app.
            </p>
          ) : (
            <>
              <p>
                Get the official release directly on your PS5. Atmosphere verifies
                its SHA-256 checksum and replaces its saved copy. Pairing,
                sources and your queue stay saved. Only manager copies you opted
                to sync are refreshed. A manually imported copy needs replacing
                if you have not enabled sync under Payload managers.
              </p>
              <div className="dialog-actions">
                <button
                  disabled={disabled || checkWait > 0}
                  onClick={() => void act("check")}
                >
                  {status.phase === "checking"
                    ? "Checking…"
                    : checkWait > 0
                      ? `Check again in ${checkWait}s`
                      : "Check for updates"}
                </button>
                {status.latestVersion && (
                  <button
                    className="primary"
                    disabled={disabled || wait > 0 || !status.checkedAt}
                    onClick={() => void act("install")}
                  >
                    {status.phase === "downloading" || status.phase === "saving"
                      ? "Installing…"
                      : status.latestVersion === status.runningVersion
                        ? "Reinstall release"
                        : "Install update"}
                  </button>
                )}
              </div>
              <p className="fine" role="status">
                {status.phase === "downloading"
                  ? `Downloading Atmosphere: ${bytes(status.received)}`
                  : status.phase === "saving"
                    ? "Saving the verified release…"
                    : wait > 0
                      ? `Please wait ${wait}s before another request.`
                      : status.phase === "checked" &&
                          status.latestVersion === status.runningVersion
                        ? "You’re running the latest published version. You can reinstall it if needed."
                        : ""}
              </p>
              {status.restartRequired && (
                <div className="notice" role="status">
                  <strong>Restart needed</strong>
                  <p>
                    The saved version starts the next time you run Atmosphere. The
                    current session stays open until you stop it.
                  </p>
                </div>
              )}
              {!confirmStop ? (
                <button
                  disabled={disabled}
                  onClick={() => setConfirmStop(true)}
                >
                  Stop Atmosphere to restart
                </button>
              ) : (
                <div className="notice">
                  <p>
                    Stop Atmosphere and pause active downloads? Then run{" "}
                    <b>/data/atmosphere/atmosphere.elf</b> (or a synced copy)
                    from your payload manager and reopen Atmosphere. Other payloads
                    keep running.
                  </p>
                  <div className="dialog-actions">
                    <button
                      disabled={disabled}
                      onClick={() => void act("stop")}
                    >
                      Stop Atmosphere
                    </button>
                    <button
                      disabled={disabled}
                      onClick={() => setConfirmStop(false)}
                    >
                      Keep running
                    </button>
                  </div>
                </div>
              )}
            </>
          )}
          {status.error && (
            <p className="form-error" role="alert">
              {status.error}
            </p>
          )}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
