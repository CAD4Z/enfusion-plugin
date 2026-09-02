/** How long after the panel regains focus a synthetic press is treated as stolen. */
export const STOLEN_PRESS_MS = 400;

/**
 * Whether a click is the press Windows returns to the focused button with the editor's focus.
 *
 * `detail` is zero for a keyboard/synthetic click and positive for a pointer click. A real pointer
 * press remains real however soon somebody makes it after bringing the panel back to the front.
 */
export function isStolenPress(detail: number, elapsed: number): boolean {
  return detail === 0 && elapsed >= 0 && elapsed < STOLEN_PRESS_MS;
}
