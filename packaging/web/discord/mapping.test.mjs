// Does the shell's URL mapping actually send the game's traffic through the
// proxy, and does it leave everything else alone?
//
// This is worth a test because the failure is silent and remote: a wrong
// prefix, or a scheme dropped on the WebSocket, does not throw -- it comes
// back as `blocked:csp` in a Discord client on somebody else's machine, with
// nothing in the build to suggest anything was wrong.
//
// The mappings are READ OUT OF shell.html and executed, rather than restated
// here. A test that declared its own copy of the config would agree with
// itself forever while the shipped page said something else.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

// attemptRemap is the exact function patchUrlMappings calls for every fetch,
// socket and XHR, so this exercises Discord's shipped rewriter rather than a
// reimplementation of it. It reads window.location.host as the proxy origin.
import { attemptRemap } from '@discord/embedded-app-sdk';


// OD_SHELL points this at a copy, which is how the suite itself gets checked:
// break the mapping in a scratch shell, and these tests must go red.
const SHELL = process.env.OD_SHELL ||
    fileURLToPath(new URL('../../../shell.html', import.meta.url));
const ISSUER = 'https://opendoctrines-net.opendoctrines.workers.dev';
const APP_ID = '1547303703370014830';
const PROXY_HOST = APP_ID + '.discordsays.com';

// The page the proxy serves the Activity from; attemptRemap reads it from here.
globalThis.window = { location: { host: PROXY_HOST } };

/**
 * Run the shell's own odActivity block, with the values CMake would stamp in
 * and a page pretending to be whatever `search` and `hostname` say.
 */
function odActivity({ search = '', hostname = PROXY_HOST,
                      issuer = ISSUER, appId = APP_ID } = {}) {
    const html = readFileSync(SHELL, 'utf8');
    const start = html.indexOf('var odActivity = (function () {');
    assert.notEqual(start, -1, 'shell.html no longer defines odActivity');
    const end = html.indexOf('\n})();', start);
    assert.notEqual(end, -1, 'could not find the end of the odActivity block');
    const src = html.slice(start, end + '\n})();'.length)
        .replace('__OD_ACCOUNT_ISSUER__', issuer)
        .replace('__OD_DISCORD_APP_ID__', appId);

    const ctx = { location: { search, hostname }, URL, URLSearchParams, console };
    vm.createContext(ctx);
    vm.runInContext(src + '\nodActivity;', ctx);
    // Cloned out of the sandbox: objects born in a vm context have that
    // context's Object prototype, and a strict deep-equal against one built
    // out here fails on the prototype alone while every value matches.
    return JSON.parse(JSON.stringify(ctx.odActivity));
}

const IN_DISCORD = { search: '?frame_id=f1&instance_id=i1' };

/** The shipped config, put through Discord's own rewriter. */
function rewrite(url, act = odActivity(IN_DISCORD)) {
    assert.equal(act.mappings.length, 1, 'expected exactly one mapping');
    return attemptRemap({ url: new URL(url), mappings: act.mappings }).toString();
}

test('the mapping is the one configured in the developer portal', () => {
    const act = odActivity(IN_DISCORD);
    assert.deepEqual(act.mappings,
        [{ prefix: '/api', target: 'opendoctrines-net.opendoctrines.workers.dev' }]);
});

test('sign-in reaches the account service through the proxy', () => {
    assert.equal(rewrite(ISSUER + '/health'),
                 'https://' + PROXY_HOST + '/api/health');
    assert.equal(rewrite(ISSUER + '/ticket'),
                 'https://' + PROXY_HOST + '/api/ticket');
});

test('a join fetches its descriptor through the proxy', () => {
    assert.equal(rewrite(ISSUER + '/session/ABCD'),
                 'https://' + PROXY_HOST + '/api/session/ABCD');
});

test('THE WEBSOCKET KEEPS ITS SCHEME -- multiplayer is wss, not https', () => {
    // If this rewrote to https:// the relay connection would fail at the
    // point a player joins a game, which is the one place nobody would look
    // for a URL-mapping bug.
    const out = rewrite('wss://opendoctrines-net.opendoctrines.workers.dev/session/ABCD/ws');
    assert.equal(out, 'wss://' + PROXY_HOST + '/api/session/ABCD/ws');
});

test('the ticket signing keys are reachable', () => {
    assert.equal(rewrite(ISSUER + '/.well-known/od-keys.json'),
                 'https://' + PROXY_HOST + '/api/.well-known/od-keys.json');
});

test("the game's own files are left alone", () => {
    // The wasm, the .data and discord-sdk.js are served from the proxy origin
    // already. Rewriting them would point the page at the account service.
    const same = 'https://' + PROXY_HOST + '/OpenDoctrines.wasm';
    assert.equal(rewrite(same), same);
});

test('an unrelated host is left alone', () => {
    assert.equal(rewrite('https://example.com/x'), 'https://example.com/x');
});

test('outside Discord the prologue does nothing', () => {
    // The same shell serves itch.io and the plain web build. Patching there
    // would rewrite working URLs to a proxy that does not exist.
    assert.equal(odActivity({ search: '', hostname: 'opendoctrines.pages.dev' }).active, false);
    assert.equal(odActivity({ search: '?frame_id=f1', hostname: PROXY_HOST }).active, false,
                 'frame_id alone is not an Activity; the SDK needs instance_id too');
    assert.equal(odActivity({ search: '?instance_id=i1' }).active, false);
    assert.equal(odActivity(IN_DISCORD).active, true);
});

test('a build with no account service maps nothing', () => {
    // A source build has an empty issuer and offers no sign-in at all. It must
    // not produce a mapping with an empty target, which matches everything.
    const act = odActivity({ ...IN_DISCORD, issuer: '' });
    assert.deepEqual(act.mappings, []);
});

test('the app id is taken from the origin when the build disagrees', () => {
    const act = odActivity({ ...IN_DISCORD, appId: '' });
    assert.equal(act.clientId, APP_ID, 'falls back to the serving origin');
    assert.equal(odActivity({ ...IN_DISCORD, hostname: 'localhost' }).originId, '');
});
