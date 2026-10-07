import { useCallback, useEffect, useState, type ReactNode } from "react";
import { CatalogueUpdates } from "./CatalogueUpdates";
import { Updates } from "./Updates";
import { api } from "./api";
import { Icon, Modal } from "./components";
import type { AutoStart as Status, System } from "./types";
function Row({
  title,
  action,
  children,
}: {
  title: string;
  action?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="storage-row autostart-row">
      <Icon name="power" />
      <div>
        <h3>{title}</h3>
        {children}
      </div>
      {action}
    </div>
  );
}
export function AutoStart({
  system,
  close,
}: {
  system: System | null;
  close: () => void;
}) {
  const [status, setStatus] = useState<Status | null>(null),
    [busy, setBusy] = useState(""),
    [stopped, setStopped] = useState(false),
    [error, setError] = useState("");
  const load = useCallback(async () => {
    try {
      setStatus(await api<Status>("/autoboot"));
    } catch (e) {
      setError((e as Error).message);
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load]);
  async function set(manager: string, enabled: boolean, integration = false) {
    setBusy(`${integration ? "integration-" : ""}${manager}`);
    setError("");
    try {
      setStatus(
        await api<Status>(integration ? "/integrations" : "/autoboot", {
          manager,
          enabled,
        }),
      );
    } catch (e) {
      setError((e as Error).message);
      void load();
    } finally {
      setBusy("");
    }
  }
  const toggle = (manager: string, enabled: boolean, unavailable = false) => (
    <button
      disabled={!!busy || unavailable}
      onClick={() => {
        void set(manager, !enabled);
      }}
    >
      {busy === manager ? "Saving…" : enabled ? "Turn off" : "Turn on"}
    </button>
  );
  const pm = status?.payloadManager,
    hb = status?.homebrewLauncher,
    files = status?.autoloadTxt?.files || [],
    paths = files.map((f) => f.path).join(", "),
    listOn = files.length > 0 && files.every((f) => f.enabled);
  const integrationButton = (
    manager: string,
    managed: boolean,
    listed: boolean,
  ) => (
    <button
      disabled={!!busy || !!status?.preferencesError}
      onClick={() => void set(manager, !(managed && listed), true)}
    >
      {busy === `integration-${manager}`
        ? "Saving…"
        : managed
          ? listed
            ? "Stop syncing"
            : "Retry setup"
          : listed
            ? "Allow updates to this copy"
            : "Add Atmosphere"}
    </button>
  );
  return (
    <Modal title="App settings" close={close}>
      <h1>App settings</h1>
      <Updates onStopped={() => setStopped(true)} />
      {!stopped && (
        <>
          <CatalogueUpdates />
          <h2>Payload managers</h2>
          <p>
            Choose where Atmosphere adds a copy and keeps it up to date. Adding a
            copy does not turn on auto-start. Stopping sync leaves existing
            copies and auto-start choices in place.
          </p>
          {system?.launcherStatus === "error" && (
            <p className="notice">
              The home-screen icon couldn’t be set up. Atmosphere still opens at port{" "}
              {system.httpPort} on this console’s IP address.
            </p>
          )}
          {!status ? (
            !error && <p className="muted">Checking your console…</p>
          ) : !status.available ? (
            <p className="notice">
              Manager integration is set up from Atmosphere on your PS5.
            </p>
          ) : (
            <>
              {status.preferencesError && (
                <p className="notice" role="alert">
                  {status.preferencesError}
                </p>
              )}
              <div className="storage-list">
                <Row
                  title="Payload Manager"
                  action={
                    pm?.installed &&
                    integrationButton("payload-manager", pm.managed, pm.listed)
                  }
                >
                  <p>
                    {!pm?.installed
                      ? "Not found on this console."
                      : pm.managed
                        ? pm.listed
                          ? "Atmosphere keeps this copy up to date when you install an update."
                          : "Your choice is saved. Retry to finish adding Atmosphere."
                        : pm.listed
                          ? "An existing copy is present. Allow updates if you want Atmosphere to manage it."
                          : "Add Atmosphere to its payload list when you choose."}
                  </p>
                </Row>
                {hb?.installed && (
                  <Row
                    title="Homebrew Launcher"
                    action={integrationButton(
                      "homebrew-launcher",
                      hb.managed,
                      hb.listed,
                    )}
                  >
                    <p>
                      {hb.managed
                        ? hb.listed
                          ? "Atmosphere keeps this menu entry up to date."
                          : "Your choice is saved. Retry to finish adding Atmosphere."
                        : hb.listed
                          ? "An existing copy is present. Allow updates if you want Atmosphere to manage it."
                          : "Add Atmosphere to its homebrew menu when you choose."}
                    </p>
                  </Row>
                )}
              </div>
              <h2>Start automatically</h2>
              <p>
                The Atmosphere icon opens Atmosphere while it’s running. Auto-start
                is a separate choice. Atmosphere leaves your manager’s global switch
                unchanged.
              </p>
              <div className="storage-list">
                <Row
                  title="Payload Manager auto-start"
                  action={
                    pm?.installed &&
                    toggle(
                      "payload-manager",
                      pm.enabled,
                      !pm.managed && !pm.enabled,
                    )
                  }
                >
                  <p>
                    {!pm?.installed
                      ? "Not found on this console."
                      : !pm.enabled
                        ? pm.managed
                          ? "Turn on to add Atmosphere to the autoload list. Your manager’s global switch controls whether it starts."
                          : "Add Atmosphere under Payload managers first, then choose whether it should start automatically."
                        : pm.switchOn
                          ? "Starts Atmosphere automatically."
                          : `Atmosphere is on its autoload list. Turn on the global Autoload switch in Payload Manager to enable startup${pm.otherEntries ? ` for Atmosphere and the other ${pm.otherEntries} ${pm.otherEntries === 1 ? "payload" : "payloads"} on that list` : ""}.`}
                  </p>
                </Row>
                <Row
                  title="autoload.txt"
                  action={files.length > 0 && toggle("autoload-txt", listOn)}
                >
                  <p>
                    {!files.length
                      ? "No autoload.txt found. Atmosphere won’t create one, because a new autoload.txt stops your autoloader from opening Payload Manager."
                      : listOn
                        ? `Starts Atmosphere from ${paths}.`
                        : `Turn on to add Atmosphere to ${paths}.`}
                  </p>
                </Row>
                {status.etaHEN?.installed && (
                  <Row title="etaHEN">
                    <p>
                      In the etaHEN Toolbox, add {status.savedPath} as a payload
                      and turn on auto start.
                    </p>
                  </Row>
                )}
              </div>
              <p className="fine">
                {status.saved
                  ? `Atmosphere is saved at ${status.savedPath}.`
                  : "Atmosphere’s saved copy is missing. Run atmosphere.elf again to save it."}
              </p>
            </>
          )}
          {error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </>
      )}
    </Modal>
  );
}
