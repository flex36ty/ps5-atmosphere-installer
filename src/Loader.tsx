import "./loader.css";
/* Opening screen: the Solid Planet mark with a light atmosphereing its ring. The ring's back half
   passes behind the planet and its front half in front, so the light reads as a moon. */
const back = "M57.58,342.17 A214 64 -22 0 1 454.42,181.83",
  front = "M454.42,181.83 A214 64 -22 0 1 57.58,342.17";
export function Loader() {
  return (
    <div className="loader" aria-hidden="true">
      <svg viewBox="0 0 512 512" width="512" height="512">
        <defs>
          <mask id="loader-behind-planet">
            <rect width="512" height="512" fill="white" />
            <circle cx="256" cy="262" r="132" fill="black" />
          </mask>
          <mask id="loader-ring-gap">
            <rect width="512" height="512" fill="white" />
            <path d={front} fill="none" stroke="black" strokeWidth="60" />
          </mask>
        </defs>
        <g mask="url(#loader-behind-planet)">
          <path className="loader-ring" d={back} />
          <path
            className="loader-light loader-light-back"
            d={back}
            pathLength={100}
          />
        </g>
        <circle
          className="loader-planet"
          cx="256"
          cy="262"
          r="118"
          mask="url(#loader-ring-gap)"
        />
        <path className="loader-ring" d={front} />
        <path
          className="loader-light loader-light-front"
          d={front}
          pathLength={100}
        />
      </svg>
    </div>
  );
}
