import {
  useLayoutEffect,
  useRef,
  type ReactNode,
} from "react";
import type { Game, Storage } from "./types";
import { bytes } from "./types";
import { Artwork } from "./Artwork";
export function Icon({
  name,
}: {
  name:
    | "search"
    | "usb"
    | "close"
    | "download"
    | "check"
    | "power"
    | "settings"
    | "back"
    | "sources";
}) {
  return (
    <svg
      width="26"
      height="26"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.6"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {name === "search" ? (
        <>
          <circle cx="10.5" cy="10.5" r="6.8" />
          <path d="m16 16 5 5" />
        </>
      ) : name === "sources" ? (
        <>
          <path d="m12 3 9 5-9 5-9-5 9-5ZM3 12l9 5 9-5M3 16l9 5 9-5" />
        </>
      ) : name === "usb" ? (
        <>
          <path d="M8 8h8v13H8zM9 8V2h6v6M11 5h2M11 12h2" />
        </>
      ) : name === "close" ? (
        <path d="m6 6 12 12M6 18 18 6" />
      ) : name === "check" ? (
        <path d="m5 12 4 4L19 6" />
      ) : name === "back" ? (
        <path d="M15 5 8 12l7 7" />
      ) : name === "power" ? (
        <path d="M12 3v9M7.5 5.8a8 8 0 1 0 9 0" />
      ) : name === "settings" ? (
        <>
          <path d="M9.67 2.5h4.66l.5 2.24 1.8 1.04 2.19-.69 2.33 4.04-1.69 1.55v2.08l1.69 1.55-2.33 4.04-2.19-.69-1.8 1.04-.5 2.24H9.67l-.5-2.24-1.8-1.04-2.19.69-2.33-4.04 1.69-1.55v-2.08L2.85 9.13l2.33-4.04 2.19.69 1.8-1.04z" />
          <circle cx="12" cy="11.72" r="3.2" />
        </>
      ) : (
        <>
          <path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5" />
        </>
      )}
    </svg>
  );
}
export function Modal({
  title,
  close,
  children,
  className = "",
}: {
  title: string;
  close: () => void;
  children: ReactNode;
  className?: string;
}) {
  const ref = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const previous = document.activeElement as HTMLElement;
    const first = ref.current?.querySelector<HTMLElement>(
      "[data-initial-focus], button, input, select",
    );
    first?.focus();
    const old = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    function trap(e: KeyboardEvent) {
      if (e.key !== "Tab") return;
      const elements = [
        ...(ref.current?.querySelectorAll<HTMLElement>(
          'button:not([disabled]),input,select,[tabindex="0"]',
        ) || []),
      ].filter((x) => x.offsetWidth);
      if (e.shiftKey && document.activeElement === elements[0]) {
        e.preventDefault();
        elements[elements.length - 1]?.focus();
      } else if (
        !e.shiftKey &&
        document.activeElement === elements[elements.length - 1]
      ) {
        e.preventDefault();
        elements[0]?.focus();
      }
    }
    document.addEventListener("keydown", trap);
    return () => {
      document.body.style.overflow = old;
      document.removeEventListener("keydown", trap);
      if (previous?.isConnected) previous.focus();
    };
  }, []);
  return (
    <div
      className="scrim"
      onClick={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      <div
        ref={ref}
        className={`modal ${className}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
      >
        <button
          className="close icon-button"
          onClick={close}
          aria-label="Close"
        >
          <Icon name="close" />
        </button>
        {children}
      </div>
    </div>
  );
}
export function GameTile({
  game,
  open,
  onFocus,
  showDate = false,
  favorite = false,
  priority = false,
  status,
}: {
  game: Game;
  open: () => void;
  onFocus?: () => void;
  showDate?: boolean;
  favorite?: boolean;
  priority?: boolean;
  status?: string;
}) {
  return (
    <button
      className="game-tile"
      data-game={game.id}
      onClick={open}
      onFocus={onFocus}
      aria-label={`View ${game.title}${status ? `, ${status}` : ""}`}
    >
      <span className="cover">
        <Artwork
          key={game.cover}
          src={game.cover}
          fallbackSrc={game.coverFallback}
          alt=""
          width="600"
          height="600"
          priority={priority}
          decoding="async"
          referrerPolicy="no-referrer"
        />
      </span>
      <span className="tile-title">{game.title}</span>
      {status && <span className="tile-collection-state">{status}</span>}
      {favorite && (
        <span className="tile-favorite" aria-label="Favourite">
          ★
        </span>
      )}
      {showDate && game.releaseDate && (
        <time className="tile-date" dateTime={game.releaseDate}>
          {new Intl.DateTimeFormat("en", {
            month: "short",
            day: "numeric",
            year: "numeric",
            timeZone: "UTC",
          }).format(new Date(game.releaseDate))}
        </time>
      )}
    </button>
  );
}
/* Load only focused artwork, regardless of the catalogue's size. */
export function Backdrop({ games, active }: { games: Game[]; active: string }) {
  const game = games.find((g) => g.id === active);
  return (
    <div className="backdrop" aria-hidden="true">
      {game && (
        <div
          key={game.hero}
          className={`backdrop-layer ${game.artworkLayout} active`}
        >
          <Artwork
            src={game.hero}
            fallbackSrc={game.heroFallback}
            alt=""
            priority
            decoding="async"
            referrerPolicy="no-referrer"
          />
        </div>
      )}
    </div>
  );
}
export function StorageRows({ drives }: { drives: Storage[] }) {
  return (
    <div className="storage-list">
      {drives.length ? (
        drives.map((d) => (
          <div className="storage-row" key={d.id}>
            <Icon name="usb" />
            <div>
              <h3>{d.label}</h3>
              <p>{d.path}</p>
              <div className="storage-meter">
                <span
                  style={{
                    width: `${Math.min(100, Math.max(0, (1 - d.freeBytes / d.totalBytes) * 100))}%`,
                  }}
                />
              </div>
              <p>
                {bytes(d.freeBytes)} free of {bytes(d.totalBytes)}
              </p>
              <p>
                {bytes(d.pendingBytes)} needed by unfinished downloads ·{" "}
                {d.projectedFreeBytes < 0
                  ? `${bytes(-d.projectedFreeBytes)} short`
                  : `${bytes(d.projectedFreeBytes)} after queue`}
              </p>
            </div>
          </div>
        ))
      ) : (
        <div className="empty compact">
          <Icon name="usb" />
          <h3>No writable storage connected</h3>
          <p>Connect your external drive to the PS5, then refresh.</p>
        </div>
      )}
      {!!drives.length && (
        <p className="fine">
          Estimates include paused and failed downloads. Other apps can change
          free space.
        </p>
      )}
    </div>
  );
}
