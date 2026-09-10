// The two pieces of Discord's Embedded App SDK the game actually uses, put on
// `window` so the shell -- which is plain script, not a module -- can reach
// them. Everything else in the SDK comes along for the ride; the bundle is
// ~150 KB against a 7 MB game, and the alternative is hand-rolling Discord's
// undocumented postMessage handshake and re-hand-rolling it when it changes.
//
// WHY THE SDK AND NOT OUR OWN REWRITER. patchUrlMappings is the easy half and
// could be written here in fifty lines; ready() is the hard half. Until it
// resolves, Discord holds its own loading screen over the iframe, so a game
// that skips the handshake runs perfectly underneath something the player
// cannot see past. That handshake is a private protocol between the SDK and
// the Discord client, and this is the copy of it that Discord maintains.
import { DiscordSDK, patchUrlMappings } from '@discord/embedded-app-sdk';

window.OD_Discord = { DiscordSDK: DiscordSDK, patchUrlMappings: patchUrlMappings };
