# Room server

The relay that online play goes through, plus a small quick-match queue and the online
tournaments. It is a [Cloudflare Worker](https://developers.cloudflare.com/workers/) with one
[Durable Object](https://developers.cloudflare.com/durable-objects/) per room code, one per
quick-match queue and one per tournament menu, all in [`src/index.js`](src/index.js). The relay
knows nothing about the game: it passes messages along, and the game does everything else (who
hosts, the match setup, the lockstep; see `smstrikers-port/src/platform/xr_net.cpp`).

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
- Any other address answers with one line of plain text; a non-WebSocket request to `/room/...`,
  `/quick/...` or `/tourney/...` gets `426`.

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

## Tournaments

Players name their team, pick a star (the captain shown as their logo), and join a tournament of 4
or of 8 from a menu that shows how full each one is. When one is full it starts: single
elimination, every match an ordinary online match in its own room code on the relay above. After
each match both headsets report the result here.

- The headset holds one WebSocket to `wss://<your worker>/tourney/<KEY>`. `KEY` is 2 to 12
  upper-case letters or digits, built by the game like the quick-match one (edition letter and
  online protocol, e.g. `P6`): only headsets with the same `KEY` see each other. Each `KEY` is its
  own Durable Object (class `Tourneys`).
- Messages are **JSON text**, at most 2 KB; binary messages and bigger ones are ignored.
- Unlike the relay, this keeps data: team ids, names and stars, the lobbies and the brackets, in the
  object's storage (key `s` for teams and lobbies, `t:<tid>` for each tournament), so nothing is
  lost when the object hibernates or restarts. A team id is never sent to anyone else.

### Headset to server

| Message | What it does |
|---|---|
| `{"t":"hello","pid":"<16 hex>","name":"...","star":3}` | Must come first. Ties this connection to the team `pid` (the headset's persistent id; it is also the team's only login, so the server never shows it to others). A team may have several connections; all get its updates (at most 4: a fifth closes the oldest, code 4000 `replaced`). The name keeps letters, digits, space and `' . -`, runs of spaces become one, it is trimmed and cut to 16; empty means `Team ` + the first 4 characters of the pid in upper case. `star` is the captain's eTeamID, 0-7 (Daisy, Donkey Kong, Luigi, Mario, Peach, Waluigi, Wario, Yoshi), `-1` for none; anything else becomes `-1`. A later hello may change the name and the star (one without them keeps them), except during an unfinished tournament, when both stay as they are. |
| `{"t":"join","size":4}` | Join the filling lobby of 4 (or 8). Refused while in a lobby or in an unfinished tournament. |
| `{"t":"leave"}` | Leave that lobby. |
| `{"t":"result","mid":"...","my":3,"their":1,"how":"played"}` | This team's report of its match: its goals and the opponent's (0-99). `how` is `played`, or `left` when the opponent dropped out mid-match. Optional `"code"`: the room the result is for; refused if the match has been replayed under another code since (a late resend). A newer report from the same side replaces the older one. |
| `{"t":"noshow","mid":"..."}` | The opponent never came. Allowed only 5 minutes after the match became ready, and only if the opponent has not reported. |
| `{"t":"ping"}` | Keepalive. Answered `{"t":"pong"}`, by Cloudflare without waking the object. Send one at least every 20-30 s while in a lobby: a lobby connection silent for 60 s is taken for a dead one (closed, code 4001 `stale`). |

### Server to headset

After a hello, and after every change, each connection gets its own state:

```json
{"t":"state",
 "lobbies":{"4":{"size":4,"names":["Red Shells"],"stars":[3]},"8":{"size":8,"names":[],"stars":[]}},
 "me":{"pid":"0123456789abcdef","name":"Red Shells","star":3,"lobby":4,"tid":null},
 "tourney":null,
 "you":-1}
```

In a tournament, `tourney` is the bracket and `you` this team's index in its `names`:

```json
{"tid":"9f3a61c2","size":4,"names":["Red Shells","Koopa FC","Boo Crew","Lance"],"stars":[3,7,-1,0],
 "rounds":[[{"mid":"9f3a61c2.0.0","a":0,"b":1,"code":"TQ4ZK8","status":"ready","readyAt":1792000000000,
             "winner":null,"score":null,"reports":{"a":null,"b":null},"replays":0}, ...],
           [{"mid":"9f3a61c2.1.0","a":null,"b":null,"code":null,"status":"waiting", ...}]],
 "champion":null,"finishedAt":null}
```

Round 0 has `size / 2` matches, each later round half as many, down to the final. `a` and `b` are
indexes into `names` (and `stars`), `null` until known. `status` is `waiting` (a team still to
come), `ready` (both known: play in room `code`) or `done`. `readyAt` is when it became ready (ms
since 1970). `score` is `[a's goals, b's goals]`, or `null` after a no-show. `reports` holds each
side's latest report, `{"my","their","how","at"}`. `replays` counts how often the match was played
again. A refused request gets `{"t":"error","msg":"..."}`.

### Rules

- **Start.** The lobby's last place filled starts the tournament: the teams are shuffled into the
  bracket, every first-round match is `ready` with a fresh code (`T` plus 5 characters from `A-Z`
  and `2-9` without `O` and `I`), and the lobby is empty for the next teams. Names and stars are
  fixed in the bracket from then on.
- **Both reports in.** The match is decided when the scores agree (one side's `my` is the other's
  `their` and back) and at least one side says `played`; so `played` + `played`, or `left` +
  `played` with the same score, count. The higher score wins. A draw, scores that differ, or two
  `left` (a broken link, each thinks the other went) mean the match is **replayed**: a new code,
  the reports cleared, `readyAt` now, `replays` up by one.
- **One report in.** If the other side sends nothing for **3 minutes** after it (the headset only
  reports a match that really ran, so after a crash one report is normal), it stands: `left` means
  the reporter wins; `played` means the higher score wins. A lone draw is replayed.
- **No-show.** A valid `noshow` (see above) wins the match for the claimer, with `score` `null`.
- **Next round.** The winner of match `i` goes to match `i / 2` (rounded down) of the next round,
  as `a` if `i` is even, else `b`; when both are there, that match is `ready` with a fresh code.
  The final's winner is the `champion`, and `finishedAt` is set.
- **After the final.** The teams keep `me.tid` (and see the result) for **30 minutes**; then the
  tournament is deleted. Once it is finished they may already join a new lobby.
- **Lobby places.** A team whose last connection closes keeps its lobby place for **90 seconds**,
  so a reconnect does not lose it. In a running tournament nothing happens on a disconnect: the
  headsets leave this menu to play their match.
- **Abandoned tournaments.** One with no report and no result for **60 minutes** is deleted and
  its teams are free again, so a bracket where both teams of a match vanished never holds its
  players (or a place under the cap) for ever.
- **Caps per KEY.** 400 known teams (past that, teams in no lobby and no tournament are forgotten,
  longest unseen first), 50 tournaments (a finished one still on show gives way to a new one; with
  50 running, the join that would start the 51st is refused), 600 connections (more get `503`).

The deadlines run on the object's alarm, set for the earliest one each time something changes.

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

[`test-tourney.mjs`](test-tourney.mjs) plays fake teams through the tournament rules on
`/tourney/T9`: lobbies, names and stars, a tournament of 4 to its champion (draws, disagreements and
two `left` replayed, `left` + `played` accepted), reconnects, a tournament of 8. The timer rules
need minutes, so the local copy has short test times (lone report 2.5 s, no-show 1 s, lobby place
1.5 s, silent connection 3 s, on show 4 s, abandoned 8 s) when it runs with `TEST_FAST` and the
connection asks with `?fast=1`, as this script does. Only `wrangler dev --var` sets `TEST_FAST`;
the deployed worker never has it, so `?fast=1` does nothing there and those checks are skipped:

```sh
npx wrangler dev --var TEST_FAST:1      # in one window
node test-tourney.mjs                   # in another; or: node test-tourney.mjs wss://<your worker>
```

## Point the game at it

The server address is one line in `smstrikers-port/src/platform/xr_net.cpp`:

```cpp
constexpr const char* kServerHost = "strikers-rooms.strikers-rooms.workers.dev";
```

Put your host name there (no `https://` and no path), rebuild the APK (see the main README) and
install it. Both players need builds that point at the same server: a headset on another server
never sees you. Keep the paths (`/room/<CODE>`, `/quick/<KEY>`, `/tourney/<KEY>`) and the message
formats as they are, or the game and the server won't understand each other.

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

Tournaments are cheap too: one request per connection to the menu, a few messages per match (the
reports), and the pings are answered without waking the object. While a team waits in a lobby the
object's alarm runs about once a minute to check for dead connections. Every change writes one or
two rows to the object's storage (the Free plan's SQLite storage has its own daily row limits; see
the pricing page). The matches themselves are ordinary rooms, counted as above.

That is why running your own copy is worth it if you play a lot: your own free quota is yours
alone.
