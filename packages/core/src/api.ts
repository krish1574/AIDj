/** Wire contract shared by the Fastify API and the mobile client. */

export const API_VERSION = 'v1';

export interface ApiSuccess<T> {
  success: true;
  data: T;
  message: string;
}

export interface ApiFailure {
  success: false;
  error: {
    code: ApiErrorCode;
    /** Safe to display to a user. Never contains internals. */
    message: string;
  };
}

export type ApiResponse<T> = ApiSuccess<T> | ApiFailure;

export type ApiErrorCode =
  | 'BAD_REQUEST'
  | 'UNAUTHORISED'
  | 'FORBIDDEN'
  | 'NOT_FOUND'
  | 'UPSTREAM_UNAVAILABLE'
  | 'INTERNAL';

export interface HealthPayload {
  service: string;
  version: string;
  uptimeSeconds: number;
  /** Result of a live round-trip to Convex, not a cached flag. */
  convex: {
    reachable: boolean;
    latencyMs: number | null;
  };
}

export interface WhoAmIPayload {
  userId: string;
  email: string | null;
}

export function isApiSuccess<T>(r: ApiResponse<T>): r is ApiSuccess<T> {
  return r.success;
}
