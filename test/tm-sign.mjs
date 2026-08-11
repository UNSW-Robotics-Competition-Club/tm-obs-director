// Reference implementation of the VEX TM Public API request signature.
//
// Mirrors vex-tm-client (out/Client.js, v1.5.3), which is the reference
// implementation for the Tournament Manager Public API. Both the golden-vector
// generator and the mock server import this, so a single definition governs
// what we pin our C++ signer against AND what the mock server accepts.
//
// src/tm/tm-signer.cpp must produce byte-identical output for the same inputs.

import { createHmac } from "node:crypto";

// Canonical string:
//   METHOD \n
//   pathname+search \n
//   token:<bearer> \n
//   host:<host> \n
//   x-tm-date:<http-date> \n
// with a trailing newline after the final component.
//
// Note `url.host` (not `hostname`): WHATWG URL keeps the port when it is not
// the scheme default, so :8080 is signed but :80 is not.
export function stringToSign({ method, url, token, tmDate }) {
    const u = new URL(url);
    return (
        [
            method,
            u.pathname + u.search,
            `token:${token}`,
            `host:${u.host}`,
            `x-tm-date:${tmDate}`,
        ].join("\n") + "\n"
    );
}

export function sign({ method, url, token, tmDate, apiKey }) {
    return createHmac("sha256", apiKey)
        .update(stringToSign({ method, url, token, tmDate }))
        .digest("hex");
}
