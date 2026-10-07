import { useEffect, useState } from "react";
import { api } from "./api";
interface Status {
  available: boolean;
  busy: boolean;
  cached: boolean;
  revision: number;
  gameCount: number;
  checkedAt: number;
  checkAfter: number;
  error: string;
}
export function CatalogueUpdates() {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    let active = true;
    let loading = false;
    const read = async () => {
      if (loading) return;
      loading = true;
      try {
        const next = await api<Status>("/catalog/updates");
        if (active) {
          setStatus(next);
          setError("");
        }
      } catch (e) {
        if (active) setError((e as Error).message);
      } finally {
        loading = false;
      }
    };
    void read();
    const timer = setInterval(() => {
      setNow(Date.now());
      void read();
    }, 2000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, []);
  async function refresh() {
    setSending(true);
    setError("");
    try {
      setStatus(await api<Status>("/catalog/updates", { action: "refresh" }));
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const wait = Math.max(
    0,
    Math.ceil(((status?.checkAfter || 0) * 1000 - now) / 1000),
  );
  return (
    <section
      className="updates-panel"
      aria-labelledby="catalogue-updates-title"
    >
      <h2 id="catalogue-updates-title">Game catalogue</h2>
      <p>
        New games arrive without reinstalling Atmosphere. Your PS5 checks on startup
        and every six hours, and keeps a saved catalogue for offline browsing.
      </p>
      {status && (
        <p className="muted">
          {status.gameCount} games · Catalogue {status.revision}
          {status.checkedAt
            ? ` · Last checked ${new Date(status.checkedAt * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}`
            : ""}
        </p>
      )}
      <div className="dialog-actions">
        <button
          disabled={!status?.available || status.busy || sending || wait > 0}
          onClick={() => void refresh()}
        >
          {sending || status?.busy
            ? "Refreshing catalogue…"
            : "Refresh catalogue"}
        </button>
        {wait > 0 && !status?.busy && (
          <span className="fine">Refresh available in {wait}s</span>
        )}
      </div>
      {status && !status.available && (
        <p className="fine">
          Refresh from Atmosphere running on your PS5. This preview uses its bundled
          catalogue.
        </p>
      )}
      {(error || status?.error) && (
        <p className="notice" role="status">
          {error || status?.error}
        </p>
      )}
    </section>
  );
}
