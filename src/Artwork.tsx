import { useEffect, useRef, useState, type ImgHTMLAttributes } from "react";
import { watchArtwork } from "./artworkVisibility";

type Props = Omit<
  ImgHTMLAttributes<HTMLImageElement>,
  "src" | "loading" | "fetchPriority" | "onError"
> & {
  src: string;
  fallbackSrc?: string | null;
  priority?: boolean;
};
export function Artwork(props: Props) {
  // A catalogue update starts a fresh attempt, even when this tile stays mounted.
  return <ArtworkImage key={`${props.src}\n${props.fallbackSrc || ""}`} {...props} />;
}
function ArtworkImage({ src, fallbackSrc, priority = false, ...props }: Props) {
  const ref = useRef<HTMLImageElement>(null);
  const [visible, setVisible] = useState(priority);
  const [attempt, setAttempt] = useState(0);
  const sources = [...new Set([src, fallbackSrc, "/atmosphere.svg"].filter((url): url is string => !!url))];
  const load = priority || visible;
  useEffect(() => {
    if (!load && ref.current)
      return watchArtwork(ref.current, () => setVisible(true));
  }, [load]);
  return (
    <img
      {...props}
      ref={ref}
      src={load ? sources[attempt] : undefined}
      loading="eager"
      fetchPriority={priority ? "high" : "auto"}
      onError={() => setAttempt(current => Math.min(current + 1, sources.length - 1))}
    />
  );
}
