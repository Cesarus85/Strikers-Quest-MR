# Room server

The relay that online play goes through, plus a small quick-match queue. It is a
[Cloudflare Worker](https://developers.cloudflare.com/workers/) with one
[Durable Object](https://developers.cloudflare.com/durable-objects/) per room code and one per
quick-match queue, about 170 lines in [`src/index.js`](src/index.js). It knows nothing about the
game: it passes messages along, and the game does everything else (who hosts, the match setup, the
lockstep; see `smstrikers-port/src/platform/xr_net.cpp`).

## What it does

- Each headset in a room holds one WebSocket to `wss://<your worker>/room/<CODE>`. `CODE` is 3 to 8
  upper-case letters or digits; the game makes sure of that before it connects.
- Every room code is its own Durable Object, so two rooms never see each other's traffic.
- **Every binary message a headset sends goes to everyone in the room, the sender included.** The
  game skips its own messages when they come back, and uses them to know the connection is alive.
- Messages must be binary and at most **4096 bytes**; text messages and bigger ones are dropped.
  The game's messages are far smaller.
- A room takes at most **4 connections**: two players, plus room for a headset that reconnected
  while its old connection has not timed out yet. A fifth connection closes the oldest one in the
  room (close code 4000).
- Nothing is stored and nothing is logged. It uses the WebSocket Hibernation API, so a room nobody
  is talking in costs nothing.
- Any other address answers with one line of plain text; a non-WebSocket request to `/room/...`
  or `/quick/...` gets `426`.

There is no login and no password. Anyone who knows the server's address and a room code can join
that room. The game ignores a third headset while two are playing, but a flood of connections
could still push a player out through the 4-connection cap.

## Quick match

For a player with nobody to play, the game waits in a queue instead of a room:

- The headset holds one WebSocket to `wss://<your worker>/quick/<KEY>`. `KEY` is 2 to 12
  upper-case letters or digits, built by the game from its edition and online protocol (`P5` =
  Plus, protocol 5; `C5` = Classic). The server never reads it: only headsets with the same `KEY`
  are matched, so builds that can't play each other never meet. Each `KEY` is its own Durable
  Object.
- Messages are **text**. The headset sends `ping` about every 5 seconds. The server sends
  `waiting <n>` (headsets searching in this queue, itself included) whenever the queue changes and
  in answer to each ping, and `match <CODE>` when it has found an opponent, followed by a close
  (code 1000, `matched`). Binary messages and anything over 64 bytes are ignored.
- **Pairing:** whenever a headset arrives or sends something, the two that have waited longest are
  paired, so the queue never holds more than one live headset per `KEY`. A headset not heard from
  for **20 seconds** is closed (code 4001, `stale`) and never paired. A queue takes at most **200**
  connections; more get `503` before the WebSocket opens.
- Both get the same fresh code: `Q` plus 5 characters from `A-Z` and `2-9` without `O` and `I`
  (for example `QM98D2`). The game then joins `wss://<your worker>/room/<CODE>` with that code, and
  the room relay above does the rest, exactly as for a code the players typed. The game should act
  on `match` as soon as it arrives rather than wait for the close, which can come a few seconds
  later.

It uses the hibernation API too, so a queue with nobody in it costs nothing; there is no timer,
since a stale headset only matters once someone else arrives, and that is when it is checked.

## Run your own

You need a free Cloudflare account and [Node.js](https://nodejs.org) 22 or newer.

```sh
cd server
npm install           # wrangler, Cloudflare's command-line tool
npx wrangler login    # opens a browser once to sign in to Cloudflare
npx wrangler deploy
```

The first deploy may ask you to pick a `workers.dev` subdomain for your account. When it finishes,
it prints your address, something like `https://strikers-rooms.<your-subdomain>.workers.dev`. The
`strikers-rooms` part is the `name` in [`wrangler.toml`](wrangler.toml); change it there if you
like.

To check it works, edit `base` at the top of [`test-room.mjs`](test-room.mjs) to your address,
or pass it after a room code, and run:

```sh
node test-room.mjs        # or: node test-room.mjs SOMECODE wss://<your worker>
```

It opens two connections to one room, has each send a few messages, checks that both heard both
(echo included) and prints the round trip.

[`test-quick.mjs`](test-quick.mjs) checks quick match: three connections to `/quick/T5` (two
must get the same `match <CODE>` and be closed, the third must keep `waiting 1`) and one to
`/quick/X5` (must not be matched). It prints PASS or FAIL. Without an address it tests a local
copy, which `npx wrangler dev` runs on port 8787:

```sh
node test-quick.mjs       # or: node test-quick.mjs wss://<your worker>
```

## Point the game at it

The server address is one line in `smstrikers-port/src/platform/xr_net.cpp`:

```cpp
constexpr const char* kServerHost = "strikers-rooms.strikers-rooms.workers.dev";
```

Put your host name there (no `https://` and no path), rebuild the APK (see the main README) and
install it. Both players need builds that point at the same server: a headset on another server
never sees you. Keep the paths (`/room/<CODE>`, `/quick/<KEY>`) and the message formats as they
are, or the game and the server won't understand each other.

## Cloudflare's free plan

Checked against Cloudflare's documentation in October 2026; prices and limits change, so see
[Workers pricing](https://developers.cloudflare.com/workers/platform/pricing/) and
[Durable Objects pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)
for the current numbers.

- **Workers Free plan:** 100,000 requests a day. Each headset connecting to a room is one request.
- **Durable Objects on the Free plan:** 100,000 requests a day and 13,000 GB-s of duration a day.
  Opening a WebSocket counts as a request. Messages coming in from headsets are counted at 20 to 1
  (20 messages = 1 request); messages going out are free. Duration is billed as 128 MB per object
  for as long as it is awake.
- **When a limit is used up**, further requests of that kind fail with an error until the daily
  reset at **00:00 UTC**. For the game that means headsets can't join a room: it keeps retrying in
  the background and every match is played offline. A match already in progress can lose its link
  too, and then stops being online after about ten seconds.

How much play that is, roughly, from the game's own send rates: in the menus each headset sends
about 4 messages a second (presence twice a second, a ping and a pong), and in a match about 32
(its input every other frame, and a state check twice a second). Two players in a match are then
about 64 incoming messages a second, about 3 requests a second after the 20 to 1 count, so around
11,500 requests per hour of online match. That makes the free plan good for something like 8 to 9
hours of online matches a day, shared by everyone who uses the same server. Duration is the looser
limit: 13,000 GB-s at 128 MB is about 28 hours a day of rooms with someone in them. These are
estimates, not measurements.

Quick match is cheap next to that: one request to join the queue, then a ping every 5 seconds
(about 36 requests per hour of searching after the 20 to 1 count). A queue with someone searching
stays awake like a room with someone in it; an empty one costs nothing.

That is why running your own copy is worth it if you play a lot: your own free quota is yours
alone.
