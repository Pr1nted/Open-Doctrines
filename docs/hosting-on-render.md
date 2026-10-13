# A tournament server on Render's free plan

A lobby that is always there, a turn a day, Modern Day, and AI countries that
answer players' letters through a free model API (Groq). Nothing to pay and no
machine of yours left on. Players join with the invite code alone.

`tests/render_sim_test.sh` runs this exact setup in Docker under the plan's
limits -- 512 MB, a tenth of a CPU, a disk wiped on every start, a new
instance started before the old one stops -- and is what "it works" below
means.

## What the free plan is, and how this copes

| The plan | What happens here |
|---|---|
| 512 MB, 0.1 CPU | The server peaks around 120 MB; Modern Day opens in about half a minute at a tenth of a CPU, and a turn a day does not notice. |
| The disk is wiped on every restart, redeploy or sleep | The campaign lives in a **private GitHub repository** (`OD_CAMPAIGN_REPO`). Every start restores it; every turn, every hour, and every shutdown pushes it. Letters are saved within seconds of being written. |
| It sleeps after 15 minutes without an inbound request | Players arrive through the relay, which is outbound, so the account service pings `/healthz` every 10 minutes (`KEEPALIVE_URLS` in `net/wrangler.toml`). |
| "Render might restart a Free web service at any time" | A restart costs about a minute. A crash (no clean stop) costs up to 15 minutes, until the dead instance's lock goes stale. |
| A redeploy starts the new instance before stopping the old one | The new one answers its health check at once and **waits**; Render then stops the old one, which saves, pushes and lets go; the new one carries on with the same code and every seat. Two servers never host the campaign at once. |
| 5 GB of bandwidth a month, counting what the service sends itself | The campaign is pushed after each turn, on shutdown, and hourly **only if something changed** (the session token renews about twice a day). Players' traffic goes through the relay. |
| 500 build minutes a month, 2-CPU builder | A build compiles the server: budget roughly 45-60 minutes per deploy. Deploy on release tags, not on every commit (`autoDeployTrigger: off`). |

## What you need first

1. **A Render account** (Hobby, no card).
2. **A Groq API key** (console.groq.com). Free, and limited per model --
   `openai/gpt-oss-20b`: 8,000 tokens a minute, 200,000 a day. A letter costs
   about 7,000 (two requests: one round of lookups, then the letter; the
   briefing travels with each), so **about 28 AI replies a day per model**. The blueprint lists two fallbacks
   (`OD_LLM_FALLBACK_MODELS`: `openai/gpt-oss-120b`, `qwen/qwen3.8-27b`), each
   with its own allowance: when one model's day is spent the next answers, so
   **about 80 replies a day** in all. The server waits out a per-minute
   limit and asks again; past every model's daily one, replies wait. Groq retires models:
   `tests/llm_endpoint_live_test.sh --from-clipboard --list-models` shows what
   a key may use now.
3. **A private GitHub repository** for the campaign, e.g. `od-campaign`, and a
   **fine-grained token** for it: that repository only, *Contents: Read and
   write*, expiring after the tournament.
4. **Your identity as a host, in that repository.** Sign in to the game on your
   own computer (*Account*), host any game once so the server credential is
   registered, then from the game's folder:

   ```bash
   OD_DATA_DIR=data tools/campaign_sync.sh init https://github.com/<you>/od-campaign.git
   ```

   It uses the git login you already have on that computer, refuses a public
   repository, and pushes `account.json` and the credential. The server then
   hosts as you, and players keep their identities across restarts.

## Create the service

1. Render -> **New** -> **Blueprint**, this repository, Blueprint path
   `deploy/server/render.yaml`.
2. It asks for the two secrets:
   - `OD_CAMPAIGN_REPO` = `https://x-access-token:<token>@github.com/<you>/od-campaign.git`
   - `OD_LLM_KEY` = the Groq key
3. Apply. The first build takes a while (see above); then the log shows
   `session open. Join code: XXXX-XXXX` and
   `language model: openai/gpt-oss-20b ... answers for every country nobody holds`.
4. Put the service's URL in the account service's keep-alive list and deploy it:

   ```toml
   # net/wrangler.toml
   KEEPALIVE_URLS = "https://od-tournament.onrender.com/healthz"
   ```

   ```bash
   cd net && npx wrangler deploy
   ```

## Running it

It is a lobby until you start it. The free plan has no Shell (it is a paid
feature), so console commands go through the campaign repository instead: put
them, one per line, in `commands.txt` on its **`commands`** branch -- GitHub's
web editor is enough -- and the server runs them within about five minutes,
once, then empties the file. The answers are in the service's **Logs**.

```text
status          who is here, who holds what
say hello all   a line in the lobby chat
start           turn 1 opens; due in 24 hours
```

Every console command works this way -- `seat`, `deadline`, `kick`, `save`.
From a terminal with `gh` instead of the web editor:

```bash
gh api -X PUT repos/<you>/od-campaign/contents/commands.txt -f branch=commands \
  -f message=start -f content="$(printf 'start\n' | base64)" \
  -f sha="$(gh api 'repos/<you>/od-campaign/contents/commands.txt?ref=commands' --jq .sha)"
```

## Before relying on the model

`tests/llm_endpoint_live_test.sh --from-clipboard` (with the Groq key copied)
runs the real server against Groq and requires a real reply to a real letter,
without the key ever being printed or logged.
