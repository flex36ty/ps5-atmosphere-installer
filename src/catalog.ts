import type { Game, Release } from "./types";

// A curated identity joins regions, sources and formats without guessing from titles.
export function groupGames(releases: Release[]): Game[] {
  const games = new Map<string, Game>();
  for (const release of releases) {
    let game = games.get(release.gameId);
    if (!game) {
      game = {
        id: release.gameId,
        title: release.title,
        genre: release.genre,
        tagline: release.tagline,
        description: release.description,
        cover: release.cover,
        hero: release.hero,
        coverFallback: release.coverFallback,
        heroFallback: release.heroFallback,
        artworkLayout: release.artworkLayout,
        publisher: release.publisher,
        releaseDate: release.releaseDate,
        addedAt: release.addedAt ?? null,
        releases: [],
      };
      games.set(game.id, game);
    }
    game.releases.push(release);
  }
  return [...games.values()];
}
