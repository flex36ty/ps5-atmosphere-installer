import { useCallback, useEffect, useRef, useState } from "react";
import { api, ApiError, setToken } from "./api";
import { groupGames } from "./catalog";
import type {
  Game,
  Release,
  Job,
  SourceId,
  SourceSettings,
  Storage,
  System,
} from "./types";
export function useAtmosphere() {
  const [games, setGames] = useState<Game[]>([]),
    [favorites, setFavorites] = useState<string[]>([]),
    [jobs, setJobs] = useState<Job[]>([]),
    [storage, setStorage] = useState<Storage[]>([]);
  const [system, setSystem] = useState<System | null>(null),
    [sources, setSources] = useState<SourceSettings | null>(null),
    [error, setError] = useState("");
  const alive = useRef(true),
    polling = useRef<Promise<void> | null>(null),
    catalogKey = useRef(""),
    localSessionLoaded = useRef(false);
  const refresh = useCallback(() => {
    if (polling.current) return polling.current;
    const request = (async () => {
      try {
        const [sys, sourceSettings] = await Promise.all([
          api<System>("/system"),
          api<SourceSettings>("/sources"),
        ]);
        if (!alive.current) return;
        const key = JSON.stringify([
          sys.version,
          sys.catalogueRevision,
          sourceSettings.noticeVersion,
          sourceSettings.acknowledged,
          sourceSettings.enabled,
          sourceSettings.options.map((source) => [
            source.id,
            source.releaseCount,
          ]),
        ]);
        if (catalogKey.current !== key) {
          const catalogue = await api<Release[]>("/catalog");
          if (!alive.current) return;
          setGames(groupGames(catalogue));
          catalogKey.current = key;
        }
        setSources(sourceSettings);
        if (
          sys.localSessionAvailable &&
          (!localSessionLoaded.current || !sys.paired)
        ) {
          const session = await api<{ token: string; pairCode: string }>(
            "/session",
          );
          if (!alive.current) return;
          setToken(session.token);
          localSessionLoaded.current = true;
          sys.paired = true;
        }
        setSystem(sys);
        if (sys.paired) {
          const [queue, drives, savedFavorites] = await Promise.all([
            api<Job[]>("/downloads"),
            api<Storage[]>("/storage"),
            api<string[]>("/favorites"),
          ]);
          if (alive.current) {
            setJobs(queue);
            setStorage(drives);
            setFavorites(savedFavorites);
          }
        } else {
          setJobs([]);
          setStorage([]);
          setFavorites([]);
        }
        setError("");
      } catch (e) {
        if (alive.current)
          setError(
            e instanceof ApiError
              ? e.message
              : "Connection lost. Reconnecting to Atmosphere…",
          );
      } finally {
        polling.current = null;
      }
    })();
    polling.current = request;
    return request;
  }, []);
  useEffect(() => {
    alive.current = true;
    void refresh();
    const timer = setInterval(() => {
      void refresh();
    }, 2000);
    return () => {
      alive.current = false;
      clearInterval(timer);
    };
  }, [refresh]);
  const pair = async (code: string) => {
    const result = await api<{ token: string }>("/pair", { code });
    setToken(result.token);
    await polling.current;
    await refresh();
  };
  const download = async (releaseId: string, storageId: string) => {
    await api("/downloads", { releaseId, storageId });
    await polling.current;
    await refresh();
  };
  const action = async (id: string, name: string, deletePartial = false) => {
    await api(`/downloads/${id}/${name}`, { deletePartial });
    await polling.current;
    await refresh();
  };
  const clearHistory = async (status: "complete" | "cancelled") => {
    await api("/downloads/clear-history", { status });
    await polling.current;
    await refresh();
  };
  const favorite = async (gameId: string, value: boolean) => {
    await api<string[]>("/favorites", { gameId, favorite: value });
    await polling.current;
    await refresh();
  };
  const saveSources = async (enabled: SourceId[], noticeVersion: number) => {
    await api("/sources", { enabled, acknowledged: true, noticeVersion });
    await polling.current;
    catalogKey.current = "";
    await refresh();
  };
  return {
    games,
    favorites,
    favorite,
    clearHistory,
    jobs,
    storage,
    system,
    sources,
    error,
    refresh,
    pair,
    download,
    action,
    saveSources,
  };
}
