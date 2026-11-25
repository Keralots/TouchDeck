// Sign-in for the portal while a PIN is set (pin_lock.h): browser sessions,
// HTTP Basic for scripts and the companion, and the gate before every request.
#pragma once
#include <WebServer.h>

// Adds the gate and the /auth routes; every URI not on the open list needs a sign-in.
void portalAuthBegin(WebServer& server);
// Wraps a multipart upload handler: the library runs it before the gate.
WebServer::THandlerFunction portalAuthUpload(WebServer::THandlerFunction handler);
// Whether the request being handled is signed in (always, without a PIN).
bool portalAuthOk();
void portalAuthTick();
