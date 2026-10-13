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
# The X, GL and sound headers are for the configure step only: it is shared with
# the game, and raylib's window layer looks for them even though the server
# never opens a window. Without them the image did not build at all. They stay
# in this stage; the image below has none of them.
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake g++ make git ca-certificates python3 \
        pkg-config libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev \
        libglu1-mesa-dev libxcursor-dev libxinerama-dev libwayland-dev libxkbcommon-dev \
 && rm -rf /var/lib/apt/lists/*
# Git's wire protocol v1 for the dependency fetches. v2 failed from inside
# Docker Desktop on a Mac -- "could not read Username", a 401 that curl to the
# same URL did not get -- while v1 worked; every git server speaks v1, so this
# costs nothing where v2 would have worked.
RUN git config --global protocol.version 1
WORKDIR /src
COPY . .
# The server target only. Mods, shaping and the rest are fetched because the
# configure step is shared with the game; nothing here links a graphics stack.
# Compile jobs, FIXED rather than $(nproc). Render's builder reports far more
# CPUs than the 2 it gives a build and caps it at 8 GB; $(nproc) started ~30
# compilers at once and the heavy game files (1-1.5 GB each) ran it out of
# memory: "Ran out of memory (used over 8GB) while building your code".
# Three is as fast as two real CPUs allow and stays near 4-5 GB.
ARG OD_BUILD_JOBS=3
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --target OpenDoctrinesServer -j"$OD_BUILD_JOBS" \
 && strip build/OpenDoctrinesServer
# OD_TEST_CLIENT=1 also builds the scripted player tests/render_sim_test.sh
# joins with. After the server's layer, so the two builds share it; and only in
# this stage, so it never reaches the image below.
ARG OD_TEST_CLIENT=0
RUN if [ "$OD_TEST_CLIENT" = 1 ]; then cmake --build build --target CampaignClient -j"$OD_BUILD_JOBS"; fi
# data/audio had to be here for configure, which checks that everything the
# game ships exists (OD_SHIPPED_DATA). A server plays no sound: 42 MB the image
# below does not need.
RUN rm -rf data/audio

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git python3 tini \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 1000 od
# The binary finds its data at <bin>/../data, so the two sit side by side.
COPY --from=build /src/build/OpenDoctrinesServer /opt/od/bin/OpenDoctrinesServer
COPY --from=build /src/data /opt/od/data
COPY tools/campaign_sync.sh deploy/server/entrypoint.sh deploy/server/health.py /opt/od/
RUN chmod +x /opt/od/entrypoint.sh /opt/od/campaign_sync.sh /opt/od/health.py \
 && mkdir -p /state && chown od:od /state
USER od
ENV OD_STATE=/state PORT=27015
EXPOSE 27015
# tini: SIGTERM from the platform reaches the server, which saves and pushes
# before it exits rather than being killed mid-write.
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/od/entrypoint.sh"]
