/* One observer for all covers. Withhold off-screen URLs even in browsers that
 * ignore native image lazy loading, so a long rail cannot flood the connection. */
const waiting = new Map<Element, () => void>();
let observer: IntersectionObserver | undefined;
let listening = false;
let frame = 0;

function cleanup() {
  if (waiting.size) return;
  observer?.disconnect();
  observer = undefined;
  if (listening) {
    window.removeEventListener("scroll", schedule, true);
    window.removeEventListener("resize", schedule);
    listening = false;
  }
  cancelAnimationFrame(frame);
  frame = 0;
}
function reveal(element: Element) {
  const load = waiting.get(element);
  if (!load) return;
  waiting.delete(element);
  observer?.unobserve(element);
  load();
  cleanup();
}
function checkVisible() {
  frame = 0;
  // Batch geometry reads before React updates. The fallback also handles rails
  // scrolling horizontally and older console browsers without IntersectionObserver.
  const visible = [...waiting.keys()].filter((element) => {
    const rect = element.getBoundingClientRect();
    return (
      rect.width > 0 &&
      rect.height > 0 &&
      rect.bottom > -180 &&
      rect.top < window.innerHeight + 180 &&
      rect.right > -180 &&
      rect.left < window.innerWidth + 180
    );
  });
  visible.forEach(reveal);
}
function schedule() {
  if (!frame) frame = requestAnimationFrame(checkVisible);
}
export function watchArtwork(element: Element, load: () => void) {
  waiting.set(element, load);
  if (typeof IntersectionObserver !== "undefined") {
    observer ||= new IntersectionObserver(
      (entries) =>
        entries.forEach((entry) => {
          if (entry.isIntersecting) reveal(entry.target);
        }),
      { rootMargin: "180px" },
    );
    observer.observe(element);
  } else {
    if (!listening) {
      window.addEventListener("scroll", schedule, {
        capture: true,
        passive: true,
      });
      window.addEventListener("resize", schedule, { passive: true });
      listening = true;
    }
    schedule();
  }
  return () => {
    waiting.delete(element);
    observer?.unobserve(element);
    cleanup();
  };
}
