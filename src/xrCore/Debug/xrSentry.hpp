#pragma once

// Optional crash reporting via sentry-native (USE_SENTRY / CMake WITH_SENTRY / MSVC UseSentry).

void xrSentry_Initialize(pcstr commandLine);
void xrSentry_Shutdown();
