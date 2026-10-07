import { useEffect } from "react";
const focusable =
  'button:not([disabled]), a[href], input:not([disabled]), select:not([disabled]), [tabindex="0"]';
export function useController(back: () => void) {
  useEffect(() => {
    function comboKey(key: string, onlyOpen = false) {
      const current = document.activeElement as HTMLElement | null;
      if (
        !current?.matches(
          onlyOpen
            ? '[role="combobox"][aria-expanded="true"]'
            : '[role="combobox"]',
        )
      )
        return false;
      current.dispatchEvent(
        new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }),
      );
      return true;
    }
    function goBack() {
      if (comboKey("Escape", true)) return;
      const close = document.querySelector<HTMLButtonElement>(
        '[role="dialog"] .close',
      );
      if (close) close.click();
      else back();
    }
    function move(direction: string) {
      const scope = document.querySelector('[role="dialog"]') || document;
      const items = [...scope.querySelectorAll<HTMLElement>(focusable)].filter(
        (el) => el.offsetWidth && el.offsetHeight && !el.closest("[inert]"),
      );
      const current = document.activeElement as HTMLElement,
        rect = current?.getBoundingClientRect();
      if (!rect || !items.includes(current)) {
        items[0]?.focus();
        return;
      }
      const x = rect.x + rect.width / 2,
        y = rect.y + rect.height / 2;
      const horizontal =
        direction === "ArrowLeft" || direction === "ArrowRight";
      const sign =
        direction === "ArrowLeft" || direction === "ArrowUp" ? -1 : 1;
      const next = items
        .filter((el) => el !== current)
        .map((el) => {
          const r = el.getBoundingClientRect(),
            dx = r.x + r.width / 2 - x,
            dy = r.y + r.height / 2 - y;
          const primary = (horizontal ? dx : dy) * sign,
            secondary = Math.abs(horizontal ? dy : dx);
          return { el, primary, score: primary + secondary * 3 };
        })
        .filter((i) => i.primary > 4)
        .sort((a, b) => a.score - b.score)[0];
      next?.el.focus();
      next?.el.scrollIntoView({
        block: "nearest",
        inline: "nearest",
        behavior: "smooth",
      });
    }
    // Rings follow the controller or keyboard; pointer and touch input hide them.
    const root = document.documentElement;
    const pointer = () => root.classList.add("pointer");
    function key(e: KeyboardEvent) {
      if (e.defaultPrevented) return;
      root.classList.remove("pointer");
      if (e.key === "Escape" || e.key === "BrowserBack") {
        e.preventDefault();
        goBack();
        return;
      }
      if ((e.target as HTMLElement).matches("input, select, textarea")) return;
      if (e.key.startsWith("Arrow")) {
        e.preventDefault();
        move(e.key);
      }
    }
    document.addEventListener("keydown", key);
    document.addEventListener("pointerdown", pointer);
    let frame = 0,
      last = 0,
      held = "";
    function tick(now: number) {
      const pad = navigator.getGamepads?.()[0];
      if (pad) {
        const command = pad.buttons[0]?.pressed
          ? "select"
          : pad.buttons[1]?.pressed
            ? "back"
            : pad.buttons[12]?.pressed || pad.axes[1] < -0.6
              ? "ArrowUp"
              : pad.buttons[13]?.pressed || pad.axes[1] > 0.6
                ? "ArrowDown"
                : pad.buttons[14]?.pressed || pad.axes[0] < -0.6
                  ? "ArrowLeft"
                  : pad.buttons[15]?.pressed || pad.axes[0] > 0.6
                    ? "ArrowRight"
                    : "";
        if (
          command &&
          (command !== held ||
            (command.startsWith("Arrow") && now - last > 240))
        ) {
          root.classList.remove("pointer");
          if (command === "select") {
            if (!comboKey("Enter"))
              (document.activeElement as HTMLElement)?.click();
          } else if (command === "back") goBack();
          else if (
            comboKey(command, command !== "ArrowUp" && command !== "ArrowDown")
          ) {
            // The dropdown owns its option navigation while it is open.
          } else if (
            document.activeElement instanceof HTMLSelectElement &&
            (command === "ArrowUp" || command === "ArrowDown")
          ) {
            const select = document.activeElement;
            const direction = command === "ArrowUp" ? -1 : 1;
            let index = select.selectedIndex + direction;
            while (
              index >= 0 &&
              index < select.options.length &&
              select.options[index].disabled
            )
              index += direction;
            if (index >= 0 && index < select.options.length) {
              select.selectedIndex = index;
              select.dispatchEvent(new Event("change", { bubbles: true }));
            }
          } else move(command);
          last = now;
        }
        held = command;
      }
      frame = requestAnimationFrame(tick);
    }
    frame = requestAnimationFrame(tick);
    return () => {
      document.removeEventListener("keydown", key);
      document.removeEventListener("pointerdown", pointer);
      cancelAnimationFrame(frame);
    };
  }, [back]);
}
