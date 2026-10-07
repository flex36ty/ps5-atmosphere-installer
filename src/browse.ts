import type { Game } from "./types";
export interface BrowseFilters {
  query: string;
  source: string;
  format: string;
  size: string;
  collection: "all" | "favorites" | "recent";
  sort: string;
}
export const defaultFilters: BrowseFilters = {
  query: "",
  source: "",
  format: "",
  size: "",
  collection: "all",
  sort: "title",
};
export function browseGames(
  games: Game[],
  filters: BrowseFilters,
  favorites: ReadonlySet<string>,
  now = Date.now(),
): Game[] {
  const query = filters.query.trim().toLowerCase();
  const matches = games.flatMap((game) => {
    if (
      query &&
      !`${game.title} ${game.releases.map((r) => r.titleId).join(" ")}`
        .toLowerCase()
        .includes(query)
    )
      return [];
    if (filters.collection === "favorites" && !favorites.has(game.id))
      return [];
    const added = game.addedAt ? Date.parse(game.addedAt) : NaN;
    if (
      filters.collection === "recent" &&
      !(added <= now && added >= now - 30 * 86400000)
    )
      return [];
    const releases = game.releases.filter((r) => {
      if (filters.source && r.sourceId !== filters.source) return false;
      if (filters.format && r.format !== filters.format) return false;
      const gb = r.sizeBytes / 1e9;
      return (
        !filters.size ||
        (filters.size === "small"
          ? gb < 10
          : filters.size === "medium"
            ? gb >= 10 && gb < 50
            : filters.size === "large"
              ? gb >= 50 && gb < 100
              : gb >= 100)
      );
    });
    return releases.length
      ? [{ game, size: Math.min(...releases.map((r) => r.sizeBytes)) }]
      : [];
  });
  matches.sort((a, b) => {
    const title = a.game.title.localeCompare(b.game.title);
    switch (filters.sort) {
      case "title-desc":
        return -title;
      case "release":
        return (
          (b.game.releaseDate || "").localeCompare(a.game.releaseDate || "") ||
          title
        );
      case "added":
        return (
          (Date.parse(b.game.addedAt || "") || 0) -
            (Date.parse(a.game.addedAt || "") || 0) || title
        );
      case "size":
        return a.size - b.size || title;
      case "size-desc":
        return b.size - a.size || title;
      default:
        return title;
    }
  });
  return matches.map((x) => x.game);
}
