#!/usr/bin/env node
//
// Register (or re-register) the /lfg slash command with Discord.
//
// ── WHY THIS IS A SCRIPT AND NOT PART OF THE WORKER ──
//
// Commands are registered once per application, not once per request, and the
// registration is a WRITE against Discord's API with the bot token. A Worker
// that registered its commands on boot would do it on every cold start, with
// the token reachable from every request, to accomplish something that changes
// about twice a year. So it is a thing a person runs, from a machine that has
// the token, when the command's shape actually changes.
//
// ── IT ASKS THE WORKER WHAT THE COMMAND IS ──
//
// The definition is not copied into this file. It is fetched from the
// deployment's /discord/command, which serves the same object lfgCommand()
// reads its options out of -- so the options Discord SHOWS and the options the
// handler READS cannot drift apart. That means the Worker has to be deployed
// first, which is the order you do this in anyway: deploy, point the
// interactions endpoint at it, then register the command.
//
//   OD_WORKER=https://<your worker> \
//   DISCORD_APP_ID=... DISCORD_BOT_TOKEN=... npm run register-lfg
//
// Add DISCORD_GUILD_ID to register it in ONE server, which takes effect
// immediately and is what you want while setting this up. Without it the
// command is global and Discord takes up to an hour to roll it out.

const worker = (process.env.OD_WORKER ?? "").replace(/\/+$/, "");
const appId = process.env.DISCORD_APP_ID;
const token = process.env.DISCORD_BOT_TOKEN;
const guild = process.env.DISCORD_GUILD_ID;

if (!worker || !appId || !token) {
    console.error("Set OD_WORKER, DISCORD_APP_ID and DISCORD_BOT_TOKEN.");
    console.error("The last two are on the application's page at discord.com/developers/applications.");
    process.exit(2);
}

const definitionResponse = await fetch(`${worker}/discord/command`);
if (!definitionResponse.ok) {
    console.error(`${worker}/discord/command answered ${definitionResponse.status}.`);
    console.error("Deploy the Worker first: npm run deploy.");
    process.exit(1);
}
const command = await definitionResponse.json();

const url = guild
    ? `https://discord.com/api/v10/applications/${appId}/guilds/${guild}/commands`
    : `https://discord.com/api/v10/applications/${appId}/commands`;

const response = await fetch(url, {
    method: "POST",
    headers: { authorization: `Bot ${token}`, "content-type": "application/json" },
    body: JSON.stringify(command),
});

if (!response.ok) {
    console.error(`Discord refused the registration (${response.status}):`);
    console.error(await response.text());
    process.exit(1);
}

console.log(guild ? `/${command.name} registered in guild ${guild}. It is usable now.`
                  : `/${command.name} registered globally. Discord takes up to an hour to roll it out.`);
