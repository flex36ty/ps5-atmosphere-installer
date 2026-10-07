const tokenKey = "atmosphere.pairing.v1";
let token = (() => {
  try {
    return localStorage.getItem(tokenKey) || "";
  } catch {
    return "";
  }
})();
export function setToken(value: string) {
  token = value;
  try {
    localStorage.setItem(tokenKey, value);
  } catch {
    /* Session-only pairing remains usable. */
  }
}
export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
    public retryAfter?: number,
  ) {
    super(message);
  }
}
export async function api<T>(path: string, body?: unknown): Promise<T> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 12000);
  try {
    const response = await fetch(`/api/v1${path}`, {
      method: body === undefined ? "GET" : "POST",
      headers: {
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...(body === undefined ? {} : { "Content-Type": "application/json" }),
      },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
    });
    const data = await response.json();
    if (!response.ok)
      throw new ApiError(
        data.error || "Atmosphere could not complete this request.",
        response.status,
        typeof data.retryAfter === "number" ? data.retryAfter : undefined,
      );
    return data as T;
  } finally {
    clearTimeout(timeout);
  }
}
