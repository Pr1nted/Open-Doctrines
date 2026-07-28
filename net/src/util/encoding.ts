// Byte/string conversions. Base64url everywhere, never plain base64: these
// values travel in URLs, in KV keys and in a token's dot-separated segments,
// and '+' '/' '=' break at least one of those in every case.

export function utf8(s: string): Uint8Array {
    return new TextEncoder().encode(s);
}

export function fromUtf8(b: Uint8Array): string {
    return new TextDecoder().decode(b);
}

export function b64urlEncode(bytes: Uint8Array): string {
    let s = "";
    for (const byte of bytes) s += String.fromCharCode(byte);
    return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

// Throws on anything that is not valid base64url. Callers on the request path
// must treat that as a 400, never as a 500 -- every one of these inputs is
// attacker-controlled.
export function b64urlDecode(s: string): Uint8Array {
    if (!/^[A-Za-z0-9_-]*$/.test(s)) throw new Error("not base64url");
    const padded = s.replace(/-/g, "+").replace(/_/g, "/") +
        "=".repeat((4 - (s.length % 4)) % 4);
    const bin = atob(padded);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

export function b64urlJson(value: unknown): string {
    return b64urlEncode(utf8(JSON.stringify(value)));
}
