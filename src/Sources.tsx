import { useEffect, useRef, useState } from "react";
import { Icon } from "./components";
import type { SourceId, SourceSettings } from "./types";

export function Sources({
  settings,
  paired,
  pair,
  save,
  saved,
}: {
  settings: SourceSettings;
  paired: boolean;
  pair: () => void;
  save: (enabled: SourceId[], noticeVersion: number) => Promise<void>;
  saved?: () => void;
}) {
  const [selected, setSelected] = useState<SourceId[]>(settings.enabled),
    [acknowledged, setAcknowledged] = useState(settings.acknowledged),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const form = useRef<HTMLFormElement>(null);
  useEffect(() => {
    form.current?.querySelector<HTMLButtonElement>(".source-card")?.focus();
  }, []);
  async function submit() {
    setPending(true);
    setError("");
    try {
      await save(selected, settings.noticeVersion);
      saved?.();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  return (
    <form
      ref={form}
      className="sources-form"
      onSubmit={(e) => {
        e.preventDefault();
        if (paired) void submit();
        else pair();
      }}
    >
      <h1>
        {settings.acknowledged ? "Download sources" : "Choose your sources"}
      </h1>
      <p className="source-intro">
        Choose where Atmosphere gets your downloads. Select one or both; you can
        change this anytime in Sources.
      </p>
      <div className="source-grid" role="group" aria-label="Download sources">
        {settings.options.map((source) => {
          const checked = selected.includes(source.id);
          return (
            <button
              key={source.id}
              type="button"
              role="checkbox"
              aria-checked={checked}
              aria-label={source.label}
              className={`source-card ${checked ? "selected" : ""}`}
              disabled={pending}
              onClick={() =>
                setSelected((current) =>
                  current.includes(source.id)
                    ? current.filter((id) => id !== source.id)
                    : [...current, source.id],
                )
              }
            >
              <span className="choice-check" aria-hidden="true">
                {checked && <Icon name="check" />}
              </span>
              {source.label}
            </button>
          );
        })}
      </div>
      <div className="source-notice">
        <h2>Before you download</h2>
        <p>
          You are responsible for checking that your downloads are lawful.
          Third-party downloads are at your own risk. Only download content you
          have permission to access and use, following applicable law and the
          provider’s terms. Public links do not prove permission, and Atmosphere does
          not guarantee the files’ safety or authenticity.
        </p>
      </div>
      <button
        type="button"
        role="checkbox"
        aria-checked={acknowledged}
        className="source-acknowledgement"
        disabled={pending}
        onClick={() => setAcknowledged((current) => !current)}
      >
        <span className="choice-check" aria-hidden="true">
          {acknowledged && <Icon name="check" />}
        </span>
        <span>
          I understand the risks and will only download content I’m legally
          entitled to access and use.
        </span>
      </button>
      {settings.acknowledged && (
        <p className="fine">
          Turning off a source hides its download options and pauses its
          unfinished downloads. Saved files stay on your drive. Re-enable the
          source and resume from Downloads when you’re ready.
        </p>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      <div className="dialog-actions">
        <button
          className="primary"
          disabled={
            pending ||
            !acknowledged ||
            (!settings.acknowledged && !selected.length)
          }
        >
          {pending
            ? "Saving…"
            : !paired
              ? "Pair to save sources"
              : "Save sources"}
        </button>
      </div>
      <p className="fine">Saved for this console and its paired devices.</p>
    </form>
  );
}
