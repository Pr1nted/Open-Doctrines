// Does the boot gate ever stop the game from starting?
//
// odDiscordGate runs from Module.preRun on EVERY web load -- itch.io, the
// plain web build, and Discord. It holds an emscripten run dependency, which
// means main() does not run until it lets go. Anything that can make it not
// let go is a game that never starts, for everybody.
//
// So the assertion in every case below is the same one: the dependency is
// released. What varies is how badly the Discord side is broken.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const SHELL = process.env.OD_SHELL ||
    fileURLToPath(new URL('../../../shell.html', import.meta.url));
const ISSUER = 'https://opendoctrines-net.opendoctrines.workers.dev';
const APP_ID = '1547303703370014830';
const PROXY_HOST = APP_ID + '.discordsays.com';

/**
 * The shell's Discord prologue, running against a fake page.
 *
 * `sdk` decides what the injected <script> tag does: 'ok' defines a working
 * window.OD_Discord, 'missing' loads but defines nothing, 'fail' 404s, and
 * 'silent' never fires either event -- the hung-network case.
 */
function boot({ search = '?frame_id=f1&instance_id=i1', sdk = 'ok',
                ready = () => Promise.resolve() } = {}) {
    const html = readFileSync(SHELL, 'utf8');
    const start = html.indexOf('var odActivity = (function () {');
    const end = html.indexOf('\nvar Module = {');
    assert.ok(start !== -1 && end > start, 'shell.html no longer has the prologue');
    const src = html.slice(start, end)
        .replace('__OD_ACCOUNT_ISSUER__', ISSUER)
        .replace('__OD_DISCORD_APP_ID__', APP_ID);

    const log = { deps: [], patched: null, warns: [], notes: [], timers: [],
                  listeners: [], beacons: [], opened: false };
    let node = null;

    const ctx = {
        URL, URLSearchParams, Promise, Error, clearTimeout,
        // Held rather than run, so a test can decide when 8 seconds pass.
        setTimeout: (fn, ms) => { log.timers.push({ fn, ms }); },
        location: { search, hostname: PROXY_HOST },
        console: {
            log: (m) => log.notes.push(String(m)),
            warn: (m) => { log.warns.push(String(m)); log.notes.push(String(m)); },
        },
        document: {
            head: { appendChild: (n) => { node = n; } },
            createElement: () => ({ src: '', async: true, onload: null, onerror: null }),
            // The paste bridge subscribes here at load, same as the beacon
            // does on window. Recorded so a test can assert it.
            addEventListener: (ev, fn) => log.listeners.push('document:' + ev),
        },
        // The usage beacon posts through this on pagehide. It is never called
        // at load -- only registering is -- but a stub that lacks it would
        // fail the moment a test exercised the beacon rather than the gate.
        navigator: { sendBeacon: (url, body) => { log.beacons.push(url); return true; } },
        open: () => { log.opened = true; },
        // The prologue registers a pagehide listener for the usage beacon.
        // Every browser has this; the stub did not, so seven tests died on
        // "window.addEventListener is not a function" -- an incomplete fake
        // page, not a broken gate. Recorded rather than swallowed, so a test
        // can assert what the prologue subscribes to.
        addEventListener: (ev, fn) => log.listeners.push(ev),
        addRunDependency: (n) => log.deps.push('+' + n),
        removeRunDependency: (n) => log.deps.push('-' + n),
        window: {},
    };
    ctx.window = ctx;
    vm.createContext(ctx);
    vm.runInContext(src + '\nodDiscordGate();', ctx);

    // Stand in for the browser fetching the script tag the prologue injected.
    if (node) {
        if (sdk === 'fail') node.onerror();
        else if (sdk !== 'silent') {
            if (sdk === 'ok') {
                ctx.OD_Discord = {
                    patchUrlMappings: (m) => { log.patched = JSON.parse(JSON.stringify(m)); },
                    DiscordSDK: function (id) { this.clientId = id; this.ready = ready; },
                };
            }
            node.onload();
        }
    }
    return { log, ctx, injected: node };
}

const settle = () => new Promise((r) => setImmediate(r));

test('outside Discord the gate never touches the run dependency', async () => {
    // itch.io and the plain web build. Taking a dependency here and failing to
    // release it would break every non-Discord player for a Discord feature.
    const { log, injected } = boot({ search: '' });
    await settle();
    assert.deepEqual(log.deps, [], 'no run dependency was taken');
    assert.equal(injected, null, 'and no SDK was fetched');
});

test('the happy path patches, hands over the app id, and releases', async () => {
    const { log, ctx } = boot();
    await settle(); await settle();
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord']);
    assert.deepEqual(log.patched,
        [{ prefix: '/api', target: 'opendoctrines-net.opendoctrines.workers.dev' }]);
    assert.equal(ctx.odDiscordPatched, true);
    assert.equal(ctx.odDiscordSdk.clientId, APP_ID);
    assert.equal(log.warns.length, 0);
});

test('a missing SDK bundle still starts the game', async () => {
    // The deploy failed to stage discord-sdk.js. The Activity will not work,
    // but the player gets a game rather than a loading bar.
    const { log } = boot({ sdk: 'fail' });
    await settle(); await settle();
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord']);
    assert.match(log.warns.join(' '), /could not load discord-sdk\.js/);
});

test('a bundle that loads but exports nothing still starts the game', async () => {
    const { log } = boot({ sdk: 'missing' });
    await settle(); await settle();
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord']);
    assert.match(log.warns.join(' '), /exported nothing usable/);
});

test('a handshake that rejects still starts the game', async () => {
    const { log } = boot({ ready: () => Promise.reject(new Error('no client')) });
    await settle(); await settle();
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord']);
    assert.match(log.warns.join(' '), /no client/);
});

test('a handshake that never answers is released by the timeout', async () => {
    // THE ONE THAT MATTERS: Discord accepts the frame and then says nothing.
    // Without the timeout this is a game that never starts and no error
    // anywhere. Time is moved rather than waited for.
    const clock = [];
    const html = readFileSync(SHELL, 'utf8');
    const start = html.indexOf('var odActivity = (function () {');
    const src = html.slice(start, html.indexOf('\nvar Module = {'))
        .replace('__OD_ACCOUNT_ISSUER__', ISSUER)
        .replace('__OD_DISCORD_APP_ID__', APP_ID);
    const deps = [];
    const ctx = {
        URL, URLSearchParams, Promise, Error,
        setTimeout: (fn, ms) => { clock.push({ fn, ms }); },
        clearTimeout: () => {},
        location: { search: '?frame_id=f1&instance_id=i1', hostname: PROXY_HOST },
        console: { log() {}, warn() {} },
        document: { head: { appendChild() {} }, createElement: () => ({}),
                    addEventListener() {} },
        // Same browser members the boot() stub needs, for the same reason:
        // the prologue registers the paste bridge and the usage beacon at
        // load, before anything about the gate has run.
        addEventListener() {},
        navigator: { sendBeacon: () => true },
        open() {},
        addRunDependency: (n) => deps.push('+' + n),
        removeRunDependency: (n) => deps.push('-' + n),
    };
    ctx.window = ctx;
    vm.createContext(ctx);
    vm.runInContext(src + '\nodDiscordGate();', ctx);

    assert.deepEqual(deps, ['+od-discord'], 'held while nothing has answered');
    assert.equal(clock.length, 1, 'exactly one timeout was armed');
    assert.ok(clock[0].ms > 0 && clock[0].ms <= 15000,
              'and it fires within a wait a person would sit through');
    clock[0].fn();
    assert.deepEqual(deps, ['+od-discord', '-od-discord'], 'the timeout releases it');
});

test('the dependency is released exactly once', async () => {
    // Both the timeout and the real answer fire in a healthy Activity: the
    // handshake completes in a second and the 8s timer is still armed behind
    // it. A second release would decrement emscripten's counter past zero and
    // start main() early on the NEXT dependency -- the .data download.
    const { log } = boot();
    await settle(); await settle();
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord'], 'released by the answer');
    assert.equal(log.timers.length, 1, 'the timeout is still armed');
    log.timers[0].fn();                       // 8 seconds pass, late
    assert.deepEqual(log.deps, ['+od-discord', '-od-discord'], 'and it does not release again');
});
