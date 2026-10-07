import { useEffect, useState } from "react";
import { api } from "./api";
import { Modal, Icon } from "./components";
import { Select } from "./Select";
import { bytes, type Storage } from "./types";

interface Settings {
  protocol: string;
  port: string;
  name: string;
  server: string;
  share: string;
  folder: string;
  username: string;
  domain: string;
  destinationFolder: string;
}
interface SMBGame {
  id: string;
  title: string;
  titleId?: string;
  minimumFirmware?: string;
  backportFiles?: boolean;
  filename: string;
  path: string;
  format: string;
  folder: boolean;
  size?: number;
  addedAt?: number;
  cover?: string;
}
interface CopyJob extends SMBGame {
  status: string;
  received: number;
  total: number;
  error: string;
  root: string;
  verification?: string;
  speedBytesPerSecond?: number;
  phase?: string;
}
interface Snapshot {
  installed?: {checking:boolean; complete:boolean; games:{titleId:string;location:string}[]};
  activeSourceId: string;
  sources: { id: string; name: string; server: string; share: string }[];
  revision: number;
  settings: Partial<Settings>;
  games: SMBGame[];
  busy: boolean;
  available: boolean;
  message: string;
  remember: boolean;
  hasPassword: boolean;
  job?: CopyJob;
}
const empty: Settings = {
  protocol: "smb",
  port: "",
  name: "",
  server: "",
  share: "",
  folder: "",
  username: "",
  domain: "",
  destinationFolder: "homebrew",
};

export function SMB({
  paired,
  pair,
  drives,
}: {
  paired: boolean;
  pair: () => void;
  drives: Storage[];
}) {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [settings, setSettings] = useState<Settings>(empty);
  const [password, setPassword] = useState("");
  const [passwordEdited, setPasswordEdited] = useState(false);
  const [remember, setRemember] = useState(false);
  const [destination, setDestination] = useState("");
  const [usbRoot, setUsbRoot] = useState(false);
  const [query, setQuery] = useState("");
  const [error, setError] = useState("");
  const [pending, setPending] = useState(false);
  const [editing, setEditing] = useState(false);
  const [selected, setSelected] = useState<SMBGame | null>(null);
  const [duplicateConfirmed,setDuplicateConfirmed]=useState("");
  const installedLocation=(game:SMBGame)=>game.titleId ? [...new Set((snapshot?.installed?.games || []).filter(g=>g.titleId.toUpperCase()===game.titleId?.toUpperCase()).map(g=>g.location || "PS5 storage"))].join(" / ") : "";
  const installedStatus=(game:SMBGame)=>installedLocation(game) ? `Installed · ${installedLocation(game)}` : !/^(PPSA|CUSA)\d{5}$/i.test(game.titleId || "") ? "Install status unknown" : snapshot?.installed?.checking ? "Checking installation…" : snapshot?.installed?.complete ? "Not installed" : "Install status unknown";
  const [sort, setSort] = useState("title");
  useEffect(() => {
    if (!paired) return;
    let stopped = false,
      timer: ReturnType<typeof setTimeout>;
    let initial = true,
      revision = -1, sourceId = "";
    async function refresh() {
      try {
        let next = await api<Snapshot>("/smb/status");
        if (initial || next.revision !== revision)
          next = await api<Snapshot>("/smb");
        revision = next.revision;
        if (stopped) return;
        setSnapshot((current) => ({
          ...next,
          games: next.games || current?.games || [],
        }));
        if (initial || next.activeSourceId !== sourceId) {
          sourceId = next.activeSourceId;
          setPassword("");
          setPasswordEdited(false);
          setSelected(null);
          setQuery("");
          setSettings({ ...empty, ...next.settings });
          setRemember(next.remember);
          setEditing(!next.settings.server);
          initial = false;
        }
      } catch (e) {
        if (!stopped) setError((e as Error).message);
      } finally {
        if (!stopped) timer = setTimeout(() => void refresh(), 1500);
      }
    }
    void refresh();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, [paired]);
  useEffect(() => {
    if (!drives.some((d) => d.id === destination))
      setDestination(drives[0]?.id || "");
  }, [drives, destination]);
  async function act(body: object) {
    setPending(true);
    setError("");
    try {
      const next = await api<Snapshot>("/smb", body);
      setSnapshot(next);
      if (next.activeSourceId !== snapshot?.activeSourceId) {
        setSettings({ ...empty, ...next.settings });
        setRemember(next.remember);
        setPassword(""); setPasswordEdited(false); setSelected(null); setQuery("");
        setEditing(!next.settings.server);
      }
      return true;
    } catch (e) {
      setError((e as Error).message);
      return false;
    } finally {
      setPending(false);
    }
  }
  const busy = pending || !!snapshot?.busy;
  const copying = snapshot?.job?.status === "copying";
  const selectedDrive = drives.find(d => d.id === destination);
  const copyToRoot = !!selectedDrive?.external && usbRoot;
  const gameList =
    (snapshot?.games.filter((g) =>
      `${g.title} ${g.titleId || ""} ${g.filename}`
        .toLowerCase()
        .includes(query.toLowerCase()),
    ) || []).sort((a, b) => {
      const difference = sort === "size" ? (b.size || 0) - (a.size || 0)
        : sort === "added" ? (b.addedAt || 0) - (a.addedAt || 0) : 0;
      return difference || a.title.localeCompare(b.title);
    });
  if (!paired)
    return (
      <section className="page">
        <h1>Servers</h1>
        <p>Pair to connect your PC or NAS and copy games.</p>
        <button className="primary" onClick={pair}>
          Pair with your PS5
        </button>
      </section>
    );
  return (
    <section className="page smb-page">
      <div className="smb-heading">
        <div>
          <p className="eyebrow">ATMOSPHERE / NETWORK LIBRARY</p>
          <h1>Your next adventure.</h1>
          <p>Your collection, ready for your console.</p>
          <div className="smb-library-meta"><span>{snapshot?.games.length || 0} games</span><span>{snapshot?.settings.server ? `${snapshot.settings.server} / ${snapshot.settings.share}` : "Connect your library"}</span></div>
        </div>
        <button disabled={busy} onClick={() => setEditing(!editing)}>
          <Icon name="settings" /> Connection
        </button>
      </div>
      <div className="smb-source-bar">
        <Select label="Server" value={snapshot?.activeSourceId || ""} disabled={busy} onChange={id => void act({action:"selectSource",sourceId:id})} options={(snapshot?.sources || []).map(s => ({value:s.id,label:s.name || (s.server ? `${s.server} / ${s.share}` : "New source")}))} />
        <button disabled={busy || (snapshot?.sources.length || 0) >= 8} onClick={() => void act({action:"addSource"})}>+ Add source</button>
      </div>
      {editing && (
        <form
          className="smb-settings"
          onSubmit={async (e) => {
            e.preventDefault();
            if (
              await act({
                action: "configure",
                ...settings,
                remember,
                ...(passwordEdited ? { password } : {}),
              })
            ) {
              setPassword("");
              setPasswordEdited(false);
              setEditing(false);
              await act({ action: "scan" });
            }
          }}
        >
          <div className="smb-fields">
            <label>Protocol<select value={settings.protocol || "smb"} disabled={busy} onChange={e => setSettings({...settings, protocol:e.target.value, port:""})}><option value="smb">SMB</option><option value="ftp">FTP</option></select></label>
            {(
              [
                ["name", "Server name (optional)", "Living room NAS"],
                ["server", "Server IP or hostname", "192.168.1.100"],
                ["port", "Port (blank for default)", settings.protocol === "ftp" ? "21" : "445"],
                ["share", "Share name (SMB only)", "Games"],
                ["folder", "Folder (FTP: from server root; SMB: inside share)", "DATA2/ps5"],
                ["username", "Username (blank for guest)", ""],
                ["domain", "Domain (optional)", ""],
                [
                  "destinationFolder",
                  "Destination folder on selected drive",
                  "homebrew",
                ],
              ] as const
            ).map(([key, label, hint]) => (
              <label key={key}>
                {label}
                <input
                  value={settings[key]}
                  placeholder={hint}
                  required={key === "server" || (key === "share" && settings.protocol !== "ftp")}
                  disabled={busy}
                  autoComplete="off"
                  onChange={(e) =>
                    setSettings({ ...settings, [key]: e.target.value })
                  }
                />
              </label>
            ))}
            <label>
              Password
              <input
                type="password"
                autoComplete="new-password"
                disabled={busy}
                value={password}
                placeholder={
                  snapshot?.hasPassword
                    ? "Saved for this session; leave unchanged"
                    : ""
                }
                onChange={(e) => {
                  setPassword(e.target.value);
                  setPasswordEdited(true);
                }}
              />
            </label>
          </div>
          <label className="smb-remember">
            <input
              type="checkbox"
              checked={remember}
              disabled={busy}
              onChange={(e) => setRemember(e.target.checked)}
            />
            Remember password on this console
          </label>
          <p className="fine">
            Remembered passwords are stored locally without encryption.
            Otherwise, enter the password again after Atmosphere restarts.
          </p>
          <button className="primary" disabled={busy}>
            Save and scan
          </button>
        </form>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {snapshot && !snapshot.available && (
        <p role="alert">The server worker is unavailable in this build.</p>
      )}
      <div className="smb-toolbar">
        <label>
          Search games
          <input
            type="search"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Title or filename"
          />
        </label>
        <div className="smb-sort"><Select label="Sort games" value={sort} onChange={setSort} options={[{value:"title",label:"Title: A–Z"},{value:"added",label:"Date added: newest first"},{value:"size",label:"Size: largest first"}]} /></div>
        <button
          disabled={busy || !snapshot?.settings.server}
          onClick={() => void act({ action: "scan" })}
        >
          {snapshot?.busy && !copying ? "Scanning…" : "Scan share"}
        </button>
      </div>
      <p className="smb-scan-status" role="status">
        {snapshot?.message || (snapshot?.settings.server ? "Library ready · Select a game to view copy options." : "Connect an SMB or FTP server to get started.")}
      </p>
      {snapshot?.job && (
        <div className="smb-progress" aria-label="server copy progress">
          <div>
            <strong>{snapshot.job.title}</strong>
            <span>{snapshot.job.status}</span>
          </div>
          <progress
            max={Math.max(1, snapshot.job.total || 0)}
            value={snapshot.job.received || 0}
          />
          <p>
            {bytes(snapshot.job.received || 0)} /{" "}
            {snapshot.job.total == null
              ? "Calculating size…"
              : bytes(snapshot.job.total)}
            {snapshot.job.verification === "sha256"
              ? " · SHA-256 verified"
              : ""}
          </p>
          {copying && <p className="smb-transfer-speed"><strong>{bytes(snapshot.job.speedBytesPerSecond || 0)}/s</strong> · {snapshot.job.phase || "Preparing"}</p>}
          {snapshot.job.error && <p role="alert">{snapshot.job.error}</p>}
          {copying ? (
            <div className="dialog-actions">
              <button
                disabled={pending}
                onClick={() => void act({ action: "pause" })}
              >
                Pause
              </button>
              <button
                disabled={pending}
                onClick={() => void act({ action: "cancel" })}
              >
                Cancel (keep partial)
              </button>
            </div>
          ) : (
            snapshot.job.status !== "complete" && (
              <button
                disabled={busy}
                onClick={() => void act({ action: "resume" })}
              >
                Resume and verify partial copy
              </button>
            )
          )}
          {snapshot.job.status !== "complete" && (
            <p className="fine">
              One server copy at a time. Starting a different copy replaces this
              resume entry; its partial files remain in .atmosphere-smb-staging on
              the destination.
            </p>
          )}
        </div>
      )}
      <div className="smb-grid">
        {gameList.map((g) => (
          <button key={g.id} className="smb-card" onClick={() => setSelected(g)} aria-label={`View ${g.title}`}>
            <span className="smb-art">
              {g.cover ? (
                <img src={g.cover} alt="" loading="lazy" />
              ) : (
                <span aria-hidden="true">
                  {g.title.slice(0, 1).toUpperCase()}
                </span>
              )}
              <span className="smb-card-prompt">× View game</span>
            </span>
            <span className="smb-card-body">
              <span className="smb-card-title">{g.title}</span>
              <span className="smb-card-meta"><span>{g.titleId}</span><span>Min FW {g.minimumFirmware || "Unknown"}</span></span>
              <span className="smb-card-meta"><span>{g.format}</span><span title="Backport folder detected; compatibility is not verified">{g.backportFiles ? "Backported" : ""}</span></span>
              <span>
                {g.size == null
                  ? "Folder"
                  : bytes(g.size)}
              </span>
              <span>{installedStatus(g)}</span>
            </span>
          </button>
        ))}
      </div>
      {selected && <Modal title={selected.title} close={() => {setSelected(null);setDuplicateConfirmed("");}} className="smb-detail">
        <div className="smb-detail-layout">
          <div className="smb-art">{selected.cover ? <img src={selected.cover} alt="" /> : <span>{selected.title.slice(0, 1)}</span>}</div>
          <div className="smb-detail-info">
            <p className="eyebrow">IN YOUR LIBRARY · {selected.format}</p>
            <h2>{selected.title}</h2>
            <p>{selected.titleId} · {selected.size == null ? "Size calculated before copying" : bytes(selected.size)}</p>
            <p>{installedStatus(selected)}</p>
            <p>Min FW {selected.minimumFirmware || "Unknown"}{selected.backportFiles ? " · Backported (files detected)" : ""}</p>
            {installedLocation(selected) && <label><input type="checkbox" checked={duplicateConfirmed===selected.id} onChange={e=>setDuplicateConfirmed(e.target.checked?selected.id:"")} /> Copy another instance of this installed title. Existing files will not be overwritten.</label>}
            <Select label="Copy destination" value={destination} onChange={setDestination} disabled={busy || !drives.length} options={drives.map(d => ({value:d.id,label:`${d.label} · ${bytes(d.freeBytes)} free`}))} />
            <Select label="Copy location" value={copyToRoot ? "root" : "folder"} onChange={value => setUsbRoot(value === "root")} disabled={busy} options={[{value:"folder",label:`Game folder /${snapshot?.settings.destinationFolder || "homebrew"}`},{value:"root",label:selectedDrive?.external ? "USB root (top level of drive)" : "USB root — select a USB drive first",disabled:!selectedDrive?.external}]} />
            {!selectedDrive?.external && <p className="smb-destination-note">To copy to USB root, choose a USB drive under Copy destination, then choose USB root under Copy location.</p>}
            {!drives.length ? <p className="smb-destination-note">No copy destination available. Storage appears when running on your PS5.</p> : <p className="smb-destination-note">Copy to {selectedDrive?.path}/{copyToRoot ? "" : `${snapshot?.settings.destinationFolder || "homebrew"}/`}{selected.filename}</p>}
            <button className="primary" disabled={busy || !destination || (!!installedLocation(selected) && duplicateConfirmed!==selected.id)} onClick={async () => { if (await act({action:"copy",gameId:selected.id,storageId:destination,usbRoot:copyToRoot,allowDuplicate:duplicateConfirmed===selected.id})) {setSelected(null);setDuplicateConfirmed("");} }}><Icon name="download" /> Copy game</button>
            {error && <p role="alert">{error}</p>}
            <p className="fine smb-detail-filename">{selected.filename}</p>
          </div>
        </div>
      </Modal>}
      {snapshot && !snapshot.busy && !gameList.length && (
        <div className="empty">
          <h2>No games found</h2>
          <p>
            Scan a share containing game folders with sce_sys/param.json or
            eboot.bin, or .ffpfsc, .ffpfs, .exfat and .img files.
          </p>
          <p>
            Titles and icons are read from supported exFAT and FFPFSC images
            and cached locally. Adjacent .json and .png files are used as
            fallbacks, for example Game.exfat.json and Game.exfat.png.
          </p>
        </div>
      )}
    </section>
  );
}
