#pragma once

// Default for local / non-CI builds. GitHub Actions (cibuild.yml) overwrites this file from
// secrets.SENTRY_DSN via misc/ci/write_xr_sentry_embed.sh before each build.
//
// At runtime, env SENTRY_DSN still overrides the embedded value (see xrSentry.cpp).
//
// Security: embedded DSN is visible in the binary; use Sentry rate limits and key rotation if abused.
//
// Example for manual embed:
// #define XRAY_SENTRY_DEFAULT_DSN_STR "https://PUBLIC_KEY@oXXXX.ingest.sentry.io/PROJECT_ID"

#define XRAY_SENTRY_DEFAULT_DSN_STR ""
