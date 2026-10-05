# An always-on server with its own language model, on Oracle Cloud's free tier

A lobby that is always there, turns of a day, and AI countries that answer
players' letters with a model running on the same machine. Free, if it fits in
Oracle's Always Free allowance. No port to open: the server hosts through the
relay, and players join with the invite code alone.

Why Oracle and not Render, Fly or Railway: their free tiers have 256–512 MB of
memory, put the service to sleep when nobody is visiting, and wipe the disk
when it restarts. That rules out a model and a campaign. Oracle's Arm VM has
24 GB, a disk that stays, and does not sleep.

---

## 1. The VM

1. Make an Oracle Cloud account (oracle.com/cloud/free). It asks for a card to
   check you are a person; Always Free resources are not charged. The **home
   region** you pick is permanent, and the free Arm machines are only free there.
2. *Compute → Instances → Create instance*:
   - **Image:** Canonical Ubuntu 24.04 (the aarch64 one is picked automatically
     for an Arm shape).
   - **Shape:** Ampere → `VM.Standard.A1.Flex`, **4 OCPUs, 24 GB**. Check
     Oracle's Always Free page for the current allowance before you go above it.
   - **SSH keys:** upload your public key (`~/.ssh/id_ed25519.pub`).
   - Everything else as it comes. The default 47 GB disk is plenty.
3. *Out of capacity* is common for the free Arm shape. Try another availability
   domain, try again later, or start with 2 OCPUs / 12 GB.

Note the public IP. `ssh ubuntu@<ip>` should get you in.

**Keep it from being reclaimed.** Oracle may reclaim an Always Free VM that
looks idle for a week (very low CPU, network *and* memory use). A server
waiting a day between turns is exactly that. The model staying loaded helps
(`server_box.sh` keeps it in memory), but the reliable fix is to upgrade the
account to *Pay As You Go*: Always Free resources stay free, and reclamation
no longer applies. Set a budget alert at $1 if you want to be told should
anything ever cost money.

## 2. Your identity as a host

The server hosts as **your account**, and its server credential decides who
every player is. Both come from your own copy of the game:

1. Open the game, sign in (*Main menu → Account*), and host any game once
   (*Multiplayer → Host*). That writes `account.json` and puts
   `serverCredential` in `config.json`, both in the game's `data/` folder.
2. Copy them to the VM:

```bash
ssh ubuntu@<ip> mkdir -p od-identity
scp data/account.json data/config.json ubuntu@<ip>:od-identity/
```

Only the account, issuer and credential are taken from `config.json`; your own
settings stay on your machine.

## 3. Set it up

```bash
ssh ubuntu@<ip>
git clone https://github.com/Pr1nted/Open-Doctrines.git
cd Open-Doctrines
tools/server_box.sh --name "Your server's name"
```

That builds the server, installs Ollama and fetches `llama3.1:8b` (the model
that understood the game best in `tools/llm_comprehend.py`), writes a campaign
in `~/od-campaign` — **Modern Day**, a turn every 24 hours, relay hosting, up
to 16 players — installs it as a service that survives reboots, and prints the
invite code. About fifteen minutes the first time, most of it the build and
the model download.

Options worth knowing (`tools/server_box.sh --help` lists all of them):

| | |
|---|---|
| `--turn-at 18:00` | turns due at 18:00 UTC every day, instead of 24 h after the start |
| `--max-players 30` | more seats |
| `--model qwen2.5:7b` | another model; `--model none` for no AI correspondents |
| `--llm-endpoint URL` | a hosted OpenAI-style API instead of a local model (put its key in `~/od-campaign/data/config.json` as `llmApiKey`) |

## 4. It is a lobby until you say so

Nothing starts by itself. Players join with the code, pick countries, and wait.
When you are ready:

```bash
tools/host_campaign.sh cmd ~/od-campaign status    # who is here, who holds what
tools/host_campaign.sh cmd ~/od-campaign start     # turn 1 opens; due in 24 h
```

`tools/host_campaign.sh log` follows the server's log; `cmd ... help` lists every
console command (seat a player, move a deadline, kick, save).

## 5. What players get from the model

Every country nobody plays answers its mail. A player writes to, say, Brazil;
the letter is posted when the turn resolves; the model reads it as Brazil's
foreign minister and replies; the reply arrives the turn after, marked as
written by a machine. What a country agrees to in its letters also leans its
government — more industry, less war, who to watch — the same as in single
player.

The model never speaks for a country a person holds, and a player's letters
are sent only to the two countries in them. On 4 Arm cores an 8B model takes
roughly half a minute per reply, which a day-long turn does not notice; the
server gathers replies as they finish rather than holding the turn for them.

## 6. Updating

```bash
cd ~/Open-Doctrines && git pull
tools/server_box.sh          # rebuilds and restarts; keeps the campaign and the code
```

Running `server_box.sh` again is safe: it reuses the identity, the campaign and
the model, rebuilds, and restarts the service. It changes only the settings you
name on that command line, so anything set by hand in `server.json` stays. The
invite code stays the same.
