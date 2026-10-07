import { useCallback, useEffect, useRef, useState } from "react";
import { api } from "./api";
import type { LibrarySnapshot, LibraryStorage } from "./types";

export function useLibrary(paired: boolean, includeStorage: boolean) {
  const [snapshot, setSnapshot] = useState<LibrarySnapshot | null>(null);
  const [storage, setStorage] = useState<LibraryStorage | null>(null);
  const [error, setError] = useState("");
  const [storageError, setStorageError] = useState("");
  const [submitting, setSubmitting] = useState(false);
  const [refreshAt, setRefreshAt] = useState(0);
  const epoch = useRef(0);
  const reads = useRef<Promise<void> | null>(null);
  const storageReads = useRef<Promise<void> | null>(null);
  const read = useCallback(
    async (manual = false) => {
      if (!paired) return;
      if (reads.current) {
        await reads.current;
        if (!manual) return;
      }
      const generation = epoch.current;
      const task = (async () => {
        try {
          const next = await api<LibrarySnapshot>(
            "/library",
            manual ? {} : undefined,
          );
          if (generation !== epoch.current) return;
          setSnapshot(next);
          setError("");
          setRefreshAt(Date.now() + next.refreshAfter * 1000);
        } catch (e) {
          if (generation === epoch.current) setError((e as Error).message);
        }
      })();
      reads.current = task;
      await task;
      if (reads.current === task) reads.current = null;
    },
    [paired],
  );
  const readStorage = useCallback(
    async (manual = false) => {
      if (!paired) return;
      if (storageReads.current) {
        await storageReads.current;
        if (!manual) return;
      }
      const generation = epoch.current;
      const task = (async () => {
        try {
          const next = await api<LibraryStorage>(
            "/library/storage",
            manual ? {} : undefined,
          );
          if (generation === epoch.current) {
            setStorage(next);
            setStorageError("");
          }
        } catch (e) {
          if (generation === epoch.current)
            setStorageError((e as Error).message);
        }
      })();
      storageReads.current = task;
      await task;
      if (storageReads.current === task) storageReads.current = null;
    },
    [paired],
  );
  useEffect(() => {
    epoch.current++;
    if (!paired) {
      setSnapshot(null);
      setStorage(null);
      setError("");
      setStorageError("");
    }
    void read();
    // One shared inventory subscription feeds cards, details, downloads and Library.
    const timer = setInterval(() => void read(), 2000);
    return () => {
      epoch.current++;
      clearInterval(timer);
    };
  }, [paired, read]);
  useEffect(() => {
    if (!paired || !includeStorage) return;
    void readStorage();
    const timer = setInterval(() => void readStorage(), 2000);
    return () => clearInterval(timer);
  }, [paired, includeStorage, readStorage]);
  useEffect(() => {
    if (!refreshAt) return;
    const timer = setTimeout(
      () => setRefreshAt(0),
      Math.max(0, refreshAt - Date.now()),
    );
    return () => clearTimeout(timer);
  }, [refreshAt]);
  const run = useCallback(
    async (action: string, fields: Record<string, unknown> = {}) => {
      if (!paired)
        throw new Error("Pair this device before using Library actions.");
      setSubmitting(true);
      try {
        const generation = epoch.current;
        const next = await api<LibrarySnapshot>("/library/actions", {
          ...fields,
          action,
          requestId: `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`,
        });
        if (generation === epoch.current) setSnapshot(next);
      } finally {
        setSubmitting(false);
      }
    },
    [paired],
  );
  const actionBusy =
    submitting || ["queued", "running"].includes(snapshot?.action?.state || "");
  return {
    snapshot,
    storage,
    error,
    storageError,
    refreshAt,
    read,
    readStorage,
    run,
    actionBusy,
    working: actionBusy || !!snapshot?.storageBusy,
    stale: !!error || !!snapshot?.stale,
    can: (capability: string) =>
      !error &&
      snapshot?.status === "ready" &&
      !snapshot.stale &&
      snapshot.capabilities.includes(capability),
  };
}
export type LibraryModel = ReturnType<typeof useLibrary>;
