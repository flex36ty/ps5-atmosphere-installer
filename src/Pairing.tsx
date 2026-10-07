import { useEffect, useState } from "react";
import { api, ApiError } from "./api";
import { Modal } from "./components";
import type { System } from "./types";

export function Pairing({
  system,
  pair,
  close,
}: {
  system: System | null;
  pair: (code: string) => Promise<void>;
  close: () => void;
}) {
  const local = !!system?.localSessionAvailable;
  const [code, setCode] = useState(""),
    [consoleCode, setConsoleCode] = useState(""),
    [error, setError] = useState(""),
    [notice, setNotice] = useState(""),
    [pairing, setPairing] = useState(false),
    [showing, setShowing] = useState(false),
    [retryAt, setRetryAt] = useState(0),
    [remaining, setRemaining] = useState(0);
  useEffect(() => {
    if (!local) return;
    let alive = true;
    // Fetch on every opening, including after a payload restart; do not reuse a cached code.
    void api<{ pairCode: string }>("/session").then(
      (session) => {
        if (alive) setConsoleCode(session.pairCode);
      },
      (e: Error) => {
        if (alive) setError(e.message);
      },
    );
    return () => {
      alive = false;
    };
  }, [local]);
  useEffect(() => {
    if (!retryAt) return;
    const update = () =>
      setRemaining(Math.max(0, Math.ceil((retryAt - Date.now()) / 1000)));
    update();
    const timer = setInterval(update, 1000);
    return () => clearInterval(timer);
  }, [retryAt]);
  function cooldown(seconds: number) {
    setRemaining(seconds);
    setRetryAt(Date.now() + seconds * 1000);
  }
  async function showCode() {
    setShowing(true);
    setError("");
    setNotice("");
    try {
      const result = await api<{ retryAfter: number }>("/pair/show", {});
      cooldown(result.retryAfter);
      setNotice(
        "Code shown on your PS5. You can show it again when the timer ends.",
      );
    } catch (e) {
      if (e instanceof ApiError && e.retryAfter) cooldown(e.retryAfter);
      setError((e as Error).message);
    } finally {
      setShowing(false);
    }
  }
  async function submit() {
    setPairing(true);
    setError("");
    try {
      await pair(code);
      close();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPairing(false);
    }
  }
  const title =
    local || system?.paired ? "Pair another device" : "Pair with your PS5";
  return (
    <Modal title={title} close={close} className="pairing-modal">
      <h1>{title}</h1>
      {local ? (
        <>
          <p>Open Atmosphere on your phone or computer and enter this code.</p>
          <div className="pairing-code persistent-code" role="status">
            <span>Pairing code</span>
            <strong
              aria-label={
                consoleCode
                  ? `Pairing code ${consoleCode.split("").join(" ")}`
                  : undefined
              }
            >
              {consoleCode || "Loading…"}
            </strong>
            <p>
              This stays on screen until you close it. The code changes when
              Atmosphere restarts.
            </p>
          </div>
          <div className="dialog-actions">
            <button className="primary" data-initial-focus onClick={close}>
              Done
            </button>
          </div>
        </>
      ) : (
        <>
          <p>
            {system?.paired
              ? "Open this Atmosphere address on the new device, then enter the code shown on your PS5."
              : "Enter the six-digit code shown in Atmosphere on your PS5."}
          </p>
          <div className="pairing-help">
            <p>
              Missed the notification? On your PS5, open Atmosphere → Pair devices to
              keep the code visible.
            </p>
            {system?.pairNotificationAvailable && (
              <button
                type="button"
                disabled={showing || remaining > 0}
                onClick={() => {
                  void showCode();
                }}
              >
                {showing
                  ? "Showing code…"
                  : remaining > 0
                    ? `Show again in ${remaining}s`
                    : "Show code on PS5"}
              </button>
            )}
            {notice && (
              <p className="pairing-notice" role="status">
                {notice}
              </p>
            )}
          </div>
          {!system?.paired && (
            <form
              onSubmit={(e) => {
                e.preventDefault();
                void submit();
              }}
            >
              <label className="field">
                Pairing code
                <input
                  data-initial-focus
                  inputMode="numeric"
                  autoComplete="one-time-code"
                  pattern="[0-9]{6}"
                  maxLength={6}
                  value={code}
                  onChange={(e) => setCode(e.target.value.replace(/\D/g, ""))}
                  required
                />
              </label>
              <div className="dialog-actions">
                <button
                  className="primary"
                  disabled={code.length !== 6 || pairing}
                >
                  {pairing ? "Pairing…" : "Pair device"}
                </button>
              </div>
            </form>
          )}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </Modal>
  );
}
