// Super Mario Strikers (Quest) room server: a relay and a quick-match queue, nothing more.
//
// Rooms: each room code is one Durable Object; every headset in the room holds a WebSocket to it,
// and each binary message one headset sends goes to every headset in the room, the sender included
// (the game hears its own messages and skips them, and uses them to know the connection is alive).
// The game itself decides everything else (who hosts, the match setup, the lockstep): see
// src/platform/xr_net.cpp in the port.
//
// Quick match: a headset with nobody to play waits in a queue, one Durable Object per KEY (the game
// builds the KEY from its edition and protocol, e.g. "P5", so only builds that can play each other
// meet; the server never reads it). The two oldest waiting headsets get the same fresh room code,
// are closed, and join that room like any other.
//
//   wss://<worker>/room/<CODE>     CODE = 3-8 letters/digits, upper case
//   wss://<worker>/quick/<KEY>     KEY  = 2-12 letters/digits, upper case
//
// Quick-match messages are text. Headset -> server: "ping" every ~5 s. Server -> headset:
// "waiting <n>" (headsets searching, itself included) after any change and in answer to a ping,
// or "match <CODE>" and then a close (1000 "matched"). Silent for 20 s = dropped (4001 "stale").
//
// Free plan: SQLite-backed Durable Objects with the WebSocket hibernation API, so an idle room or
// queue costs nothing.

const MAX_PER_ROOM = 4; // two players, plus room for a reconnect overlapping a stale socket
const MAX_PER_QUEUE = 200; // far more than ever wait at once; only stops a flood
const STALE_MS = 20000; // the game pings every ~5 s, so this is three or four missed pings
const CODE_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O/1/I; 32 chars, so byte % 32 is fair

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const room = url.pathname.match(/^\/room\/([A-Z0-9]{3,8})$/);
    const quick = url.pathname.match(/^\/quick\/([A-Z0-9]{2,12})$/);
    if (!room && !quick) {
      return new Response("Super Mario Strikers Quest room server\n", {
        headers: { "content-type": "text/plain" },
      });
    }
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected a WebSocket upgrade\n", { status: 426 });
    }
    const ns = room ? env.ROOMS : env.QUEUES;
    return ns.get(ns.idFromName((room || quick)[1])).fetch(request);
  },
};

export class Room {
  constructor(state, env) {
    this.state = state;
  }

  async fetch(request) {
    const open = this.state.getWebSockets();
    if (open.length >= MAX_PER_ROOM) {
      // The oldest goes: a headset that reconnected leaves its dead socket behind for a while.
      // r44: found by when it came (the attachment), not by place: getWebSockets() is not
      // oldest-first, and taking [0] could drop the fresh reconnect and keep the dead one.
      let oldest = open[0];
      for (const ws of open) {
        if (since_of(ws) < since_of(oldest)) oldest = ws;
      }
      try {
        oldest.close(4000, "room full, oldest dropped");
      } catch (e) {}
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    server.serializeAttachment({ since: Date.now() });
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (typeof message === "string" || message.byteLength > 4096) return; // game messages are small binary
    for (const peer of this.state.getWebSockets()) {
      try {
        peer.send(message);
      } catch (e) {}
    }
  }

  webSocketClose(ws, code, reason, wasClean) {
    try {
      ws.close(code, reason);
    } catch (e) {}
  }

  webSocketError(ws, error) {
    try {
      ws.close(1011, "error");
    } catch (e) {}
  }
}

export class Matchmaker {
  constructor(state, env) {
    this.state = state;
  }

  async fetch(request) {
    if (this.state.getWebSockets().length >= MAX_PER_QUEUE) {
      return new Response("queue full\n", { status: 503 });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    const now = Date.now();
    // The attachment survives hibernation, unlike anything kept on `this`.
    server.serializeAttachment({ since: now, seen: now });
    this.update();
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (typeof message !== "string" || message.length > 64) return; // the game only sends "ping"
    const a = ws.deserializeAttachment();
    a.seen = Date.now();
    ws.serializeAttachment(a);
    this.update();
  }

  webSocketClose(ws, code, reason, wasClean) {
    try {
      ws.close(1000, "bye");
    } catch (e) {}
    this.update(ws);
  }

  webSocketError(ws, error) {
    try {
      ws.close(1011, "error");
    } catch (e) {}
    this.update(ws);
  }

  // Drop the stale, pair the oldest live sockets two by two, and tell whoever is left how many are
  // searching. Runs on every arrival, message and departure. There is no timer (it would keep the
  // object awake): a stale socket only matters once someone else arrives, and that runs this.
  update(gone) {
    const now = Date.now();
    const live = [];
    for (const ws of this.state.getWebSockets()) {
      if (ws === gone || ws.readyState !== WebSocket.OPEN) continue; // closing already
      const { since, seen } = ws.deserializeAttachment();
      if (now - Math.max(since, seen) > STALE_MS) {
        try {
          ws.close(4001, "stale");
        } catch (e) {}
      } else {
        live.push({ ws, since });
      }
    }
    live.sort((x, y) => x.since - y.since); // getWebSockets() is not oldest-first (wrangler dev: newest)
    while (live.length >= 2) {
      const code = roomCode();
      for (const { ws } of live.splice(0, 2)) {
        try {
          ws.send("match " + code);
          ws.close(1000, "matched");
        } catch (e) {}
      }
    }
    for (const { ws } of live) {
      try {
        ws.send("waiting " + live.length);
      } catch (e) {}
    }
  }
}

// When a socket was accepted; 0 for one accepted before r44 (no attachment), so it counts as oldest.
function since_of(ws) {
  try {
    const a = ws.deserializeAttachment();
    return a && typeof a.since === "number" ? a.since : 0;
  } catch (e) {
    return 0;
  }
}

// "Q" (for quick) + 5 random characters: 32^5 = 33 million codes, so two matches never share a room.
function roomCode() {
  let code = "Q";
  for (const b of crypto.getRandomValues(new Uint8Array(5))) code += CODE_CHARS[b % CODE_CHARS.length];
  return code;
}
