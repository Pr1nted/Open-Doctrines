# syntax=docker/dockerfile:1.6
#
# The OpenDoctrines dedicated server, in a container, for hosting a campaign on
# a machine you do not own -- a free Render web service, or any Docker host.
#
#   docker build -f deploy/server/Dockerfile -t od-server .
#   docker run -p 27015:27015 -e OD_CAMPAIGN_REPO=... od-server
#
# Built from THIS source tree, so the server always matches the game you
# built it beside. Everything the server writes lives under /state, which a
# free platform wipes on every restart; deploy/server/entrypoint.sh restores it
# from a private git repository first (tools/campaign_sync.sh) and the server
# pushes it back after every turn. See docs/tournaments.md.

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake g++ make git ca-certificates python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The server target only. Mods, shaping and the rest are fetched because the
# configure step is shared with the game; nothing here links a graphics stack.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target OpenDoctrinesServer -j"$(nproc)" \
 && strip build/OpenDoctrinesServer

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git python3-minimal tini \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 1000 od
# The binary finds its data at <bin>/../data, so the two sit side by side.
COPY --from=build /src/build/OpenDoctrinesServer /opt/od/bin/OpenDoctrinesServer
COPY --from=build /src/data /opt/od/data
COPY tools/campaign_sync.sh deploy/server/entrypoint.sh /opt/od/
RUN chmod +x /opt/od/entrypoint.sh /opt/od/campaign_sync.sh \
 && mkdir -p /state && chown od:od /state
USER od
ENV OD_STATE=/state PORT=27015
EXPOSE 27015
# tini: SIGTERM from the platform reaches the server, which saves and pushes
# before it exits rather than being killed mid-write.
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/od/entrypoint.sh"]
