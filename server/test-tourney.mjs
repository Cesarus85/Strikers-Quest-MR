// Tournaments: fake teams on /tourney/T9 go through the server's rules one by one; every check
// prints ok or BAD, and PASS at the end if all were ok.
//   node test-tourney.mjs [base]      base defaults to ws://127.0.0.1:8787 (npx wrangler dev)
// The timer rules (3 min lone report, 5 min no-show, 90 s lobby grace, 60 s dead lobby socket,
// 30 min on show) need the short test times: run the local copy as
//   npx wrangler dev --var TEST_FAST:1
// This script asks for them with ?fast=1, which only such a copy honours (the state then says
// "fast": true). Against any other server those checks are skipped, not failed.
import { randomBytes } from "node:crypto";

const base = (process.argv[2] || "ws://127.0.0.1:8787").replace(/\/+$/, "");
const KEY = "T9";
const url = `${base}/tourney/${KEY}?fast=1`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const CODE = /^T[A-HJ-NP-Z2-9]{5}$/;

const results = [];
function check(what, ok, detail = "") {
  results.push(!!ok);
  console.log(`${ok ? "ok  " : "BAD "} ${what}${ok || !detail ? "" : `  (${detail})`}`);
  return !!ok;
}
function skip(what) {
  console.log(`--   ${what} (skipped: the server is not in test mode)`);
}
async function until(pred, ms = 3000) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(20)) if (pred()) return true;
  return !!pred();
}

// ---------------------------------------------------------------------------------------------
// A fake headset: one team (pid + name), one socket at a time (more when a test opens them).
// ---------------------------------------------------------------------------------------------

const sockets = [];
function open({ ping = true } = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.got = [];
    ws.errors = [];
    ws.state = null;
    ws.closed = null;
    ws.onmessage = (e) => {
      let m;
      try {
        m = JSON.parse(String(e.data));
      } catch {
        return;
      }
      ws.got.push(m);
      if (m.t === "state") ws.state = m;
      if (m.t === "error") ws.errors.push(m.msg);
    };
    ws.onopen = () => resolve(ws);
    ws.onerror = () => reject(new Error("could not connect to " + url));
    ws.onclose = (e) => {
      ws.closed = `${e.code} ${e.reason}`;
      clearInterval(ws.pinger);
    };
    // The keepalive, as the game sends it (the server must take a lobby socket for dead without).
    if (ping) ws.pinger = setInterval(() => ws.readyState === WebSocket.OPEN && ws.send('{"t":"ping"}'), 1000);
    sockets.push(ws);
  });
}

class Team {
  constructor(name, pid = randomBytes(8).toString("hex"), star = -1) {
    this.name = name;
    this.pid = pid;
    this.star = star; // the captain on the bracket: eTeamID 0-7, -1 none
    this.ws = null;
  }
  async connect(opts) {
    this.ws = await open(opts);
    return this.ws;
  }
  get st() {
    return this.ws.state;
  }
  send(m, ws = this.ws) {
    ws.send(JSON.stringify(m));
  }
  // A hello always gets a state back (or an error).
  async hello(name = this.name, ws = this.ws, extra = {}) {
    const n = ws.got.length;
    this.send({ t: "hello", pid: this.pid, name, star: this.star, ...extra }, ws);
    await until(() => ws.got.slice(n).some((m) => m.t === "state" || m.t === "error"));
    return ws.state;
  }
  // Send, and wait for the error it should earn: its text, or null if none came.
  async refused(m) {
    const n = this.ws.errors.length;
    this.send(m);
    await until(() => this.ws.errors.length > n, 1500);
    return this.ws.errors[n] ?? null;
  }
  report(m, my, their, how = "played") {
    this.send({ t: "result", mid: m.mid, my, their, how });
  }
}

async function team(name, opts, star = -1) {
  const t = new Team(name, undefined, star);
  await t.connect(opts);
  await t.hello();
  return t;
}

// New teams join the lobby of `size` one by one until it starts; they come back once all of them
// see the tournament.
async function fill(size, names, stars = []) {
  const teams = [];
  for (const [k, name] of names.entries()) {
    const t = await team(name, undefined, stars[k] ?? -1);
    t.send({ t: "join", size });
    await until(() => t.st.me.lobby === size || t.st.tourney);
    teams.push(t);
  }
  await until(() => teams.every((t) => t.st.tourney && t.st.tourney.size === size));
  return teams;
}

const match = (st, r, i) => st?.tourney?.rounds?.[r]?.[i];
const byIndex = (teams, idx) => teams.find((t) => t.st.you === idx);
const lobbyNames = (st, size) => st?.lobbies?.[size]?.names ?? [];

// ---------------------------------------------------------------------------------------------
// 0. Hello, names, the route
// ---------------------------------------------------------------------------------------------

const http = base.replace(/^ws/, "http");
check("plain GET /tourney/T9 is 426", (await fetch(`${http}/tourney/T9`)).status === 426);
check("lower-case /tourney/t9 is not a tournament (200 text)", (await fetch(`${http}/tourney/t9`)).status === 200);

const obs = await team("  Obs<>erver!!   Team  ", undefined, 3);
const fast = obs.st?.fast === true;
console.log(fast ? "(server in test mode: short timers)" : "(server not in test mode: timer checks skipped)");
check(
  "hello gets a state with both lobbies (names + stars)",
  obs.st?.t === "state" && obs.st.lobbies["4"]?.size === 4 && obs.st.lobbies["8"]?.size === 8 && Array.isArray(obs.st.lobbies["4"].stars) && Array.isArray(obs.st.lobbies["8"].stars)
);
check("name cleaned: 'Observer Team'", obs.st?.me?.name === "Observer Team", obs.st?.me?.name);
check("star 3 in me", obs.st?.me?.star === 3, obs.st?.me?.star);
check("no tournament, you = -1", obs.st?.tourney === null && obs.st?.you === -1 && obs.st?.me?.tid === null && obs.st?.me?.lobby === null);
check("pid echoed back in me", obs.st?.me?.pid === obs.pid);
{
  const t = new Team("", "ab12" + randomBytes(6).toString("hex"));
  await t.connect();
  check("join before hello refused", /hello/.test((await t.refused({ t: "join", size: 4 })) || ""));
  await t.hello("", t.ws, { star: undefined });
  check("empty name -> 'Team AB12'", t.st?.me?.name === "Team AB12", t.st?.me?.name);
  check("no star -> -1", t.st?.me?.star === -1, t.st?.me?.star);
  await t.hello("Super Long Team Name Here", t.ws, { star: 7 });
  check("later hello renames, cut to 16: 'Super Long Team'", t.st?.me?.name === "Super Long Team", t.st?.me?.name);
  check("later hello changes the star: 7", t.st?.me?.star === 7, t.st?.me?.star);
  await t.hello(undefined, t.ws, { name: undefined, star: undefined });
  check("hello without name or star keeps both", t.st?.me?.name === "Super Long Team" && t.st?.me?.star === 7);
  await t.hello("Super Long Team", t.ws, { star: 8 });
  check("star 8 -> -1", t.st?.me?.star === -1, t.st?.me?.star);
  await t.hello("Super Long Team", t.ws, { star: "3" });
  check("star \"3\" (not an int) -> -1", t.st?.me?.star === -1, t.st?.me?.star);
  const bad = new Team("x", "not-hex");
  await bad.connect();
  check("bad pid refused", /pid/.test((await bad.refused({ t: "hello", pid: "not-hex", name: "x" })) || ""));
  const before = obs.ws.got.length;
  obs.ws.send('{ "t": "ping" }');
  obs.ws.send('{"t":"ping"}');
  await until(() => obs.ws.got.slice(before).filter((m) => m.t === "pong").length >= 2, 1500);
  check("pings answered with pong (exact and spaced)", obs.ws.got.slice(before).filter((m) => m.t === "pong").length >= 2);
  t.ws.close();
  bad.ws.close();
}
check(
  "both lobbies empty at the start",
  await until(() => lobbyNames(obs.st, 4).length === 0 && lobbyNames(obs.st, 8).length === 0, fast ? 6000 : 1000),
  "leftovers from an earlier run? wait 90 s and run again"
);

// ---------------------------------------------------------------------------------------------
// 1. A lobby: join, see it, rename in it, leave
// ---------------------------------------------------------------------------------------------

{
  const l = await team("Leaver", undefined, 5);
  l.send({ t: "join", size: 4 });
  await until(() => l.st.me.lobby === 4);
  check("join: me.lobby = 4", l.st.me.lobby === 4);
  check("others see the lobby of 4 with the name", await until(() => lobbyNames(obs.st, 4).includes("Leaver")));
  check("and its star (stars parallel to names)", JSON.stringify(obs.st.lobbies["4"].stars) === "[5]", JSON.stringify(obs.st.lobbies["4"]));
  check("join again refused", /already/.test((await l.refused({ t: "join", size: 8 })) || ""));
  check("join size 5 refused", /size/.test((await l.refused({ t: "join", size: 5 })) || ""));
  await l.hello("Leaver Two", l.ws, { star: 2 });
  check("rename in a lobby shows in the lobby", await until(() => lobbyNames(obs.st, 4).includes("Leaver Two")));
  check("new star in a lobby shows in the lobby", JSON.stringify(obs.st.lobbies["4"].stars) === "[2]", JSON.stringify(obs.st.lobbies["4"]));
  l.send({ t: "leave" });
  await until(() => l.st.me.lobby === null);
  check("leave: me.lobby = null", l.st.me.lobby === null);
  check("leave: gone from the lobby", await until(() => lobbyNames(obs.st, 4).length === 0));
  check("leave again refused", /not in a lobby/.test((await l.refused({ t: "leave" })) || ""));
  l.ws.close();
}

// ---------------------------------------------------------------------------------------------
// 2. Four teams: the tournament, replays, results, the final, the champion
// ---------------------------------------------------------------------------------------------

{
  const four = await fill(4, ["Alpha", "Bravo", "Charlie", "Delta"], [0, 4, 7, -1]);
  const st0 = four[0].st;
  const T = st0.tourney;
  check(
    "tournament stars parallel to names",
    Array.isArray(T.stars) && T.stars.length === 4 && four.every((t) => T.stars[t.st.you] === t.star),
    JSON.stringify({ names: T.names, stars: T.stars })
  );
  check("observer saw the lobby of 4 at 3 names", obs.ws.got.some((m) => m.t === "state" && lobbyNames(m, 4).length === 3));
  check("4th join starts the tournament for all 4", four.every((t) => t.st.tourney && t.st.tourney.tid === T.tid && t.st.me.tid === T.tid));
  check("lobby of 4 empty again", await until(() => lobbyNames(obs.st, 4).length === 0) && four.every((t) => t.st.me.lobby === null));
  check("observer has no tournament", obs.st.tourney === null);
  check("size 4, rounds of 2 and 1", T.size === 4 && T.rounds.length === 2 && T.rounds[0].length === 2 && T.rounds[1].length === 1);
  const r0 = T.rounds[0];
  check(
    "both semis ready, with T+5 codes, readyAt, no reports",
    r0.every((m) => m.status === "ready" && CODE.test(m.code) && typeof m.readyAt === "number" && !m.reports.a && !m.reports.b && m.replays === 0),
    JSON.stringify(r0)
  );
  check("the two semi codes differ", r0[0].code !== r0[1].code);
  check("semis are 0v1 and 2v3", r0[0].a === 0 && r0[0].b === 1 && r0[1].a === 2 && r0[1].b === 3);
  check("final waiting, empty", T.rounds[1][0].status === "waiting" && T.rounds[1][0].a === null && T.rounds[1][0].b === null && T.rounds[1][0].code === null);
  check("you: each team its own index, names[you] = its name", four.map((t) => t.st.you).sort().join() === "0,1,2,3" && four.every((t) => T.names[t.st.you] === t.name));
  check("no pid in anyone's state but its own", four.every((t) => four.every((o) => o === t || !JSON.stringify(t.st).includes(o.pid))));
  check("champion and finishedAt null", T.champion === null && T.finishedAt === null);
  check("join while playing refused", /tournament/.test((await four[0].refused({ t: "join", size: 4 })) || ""));
  await four[0].hello("Renamed", four[0].ws, { star: 6 });
  check("rename refused in a tournament (name kept)", four[0].st.me.name === "Alpha" && four[0].st.tourney.names[four[0].st.you] === "Alpha");
  check("star change refused in a tournament (star kept)", four[0].st.me.star === 0 && four[0].st.tourney.stars[four[0].st.you] === 0);

  // Reconnect: a new socket with the same pid gets everything back.
  const a = four[0];
  const you = a.st.you;
  a.ws.close();
  await until(() => a.ws.closed !== null || a.ws.readyState === WebSocket.CLOSED, 2000);
  await a.connect();
  await a.hello();
  check("reconnect, same pid: same tournament, same index", a.st.tourney?.tid === T.tid && a.st.you === you && a.st.me.tid === T.tid);
  // A second socket for one team: both get updates.
  const b = four[1];
  const b2 = await open();
  await b.hello(b.name, b2);
  check("second socket of a team gets its state", b2.state?.tourney?.tid === T.tid && b2.state.you === b.st.you);

  // Semi 0: a draw, a disagreement, two "left", "left" against another score: all replayed.
  const sa = byIndex(four, r0[0].a);
  const sb = byIndex(four, r0[0].b);
  const replayed = async (what, ra, rb, n) => {
    const m = match(sa.st, 0, 0);
    sa.report(m, ...ra);
    await until(() => match(sa.st, 0, 0).reports.a);
    sb.report(m, ...rb);
    await until(() => match(sa.st, 0, 0).code !== m.code);
    const now = match(sa.st, 0, 0);
    check(
      `${what}: replayed (new code, ready, reports cleared, replays ${n})`,
      now.code !== m.code && CODE.test(now.code) && now.status === "ready" && !now.reports.a && !now.reports.b && now.replays === n && now.readyAt >= m.readyAt,
      JSON.stringify(now)
    );
    return m;
  };
  check("first report shows in the match", await (async () => {
    const m = match(sa.st, 0, 0);
    sa.report(m, 1, 1);
    return until(() => match(sb.st, 0, 0).reports.a?.my === 1 && match(sb.st, 0, 0).reports.b === null);
  })());
  {
    const m = match(sa.st, 0, 0);
    sb.report(m, 1, 1);
    await until(() => match(sa.st, 0, 0).code !== m.code);
    const now = match(sa.st, 0, 0);
    check("draw 1-1: replayed (new code, ready, reports cleared, replays 1)", now.code !== m.code && CODE.test(now.code) && now.status === "ready" && !now.reports.a && !now.reports.b && now.replays === 1, JSON.stringify(now));
  }
  const old = await replayed("disagreement (both say 2-1)", [2, 1], [2, 1], 2);
  await replayed("both 'left' (a broken link)", [0, 0, "left"], [0, 0, "left"], 3);
  await replayed("'left' 0-1 against 'played' 2-1 (scores differ)", [0, 1, "left"], [2, 1, "played"], 4);
  check("report with an old code refused", /replayed/.test((await sa.refused({ t: "result", mid: old.mid, code: old.code, my: 3, their: 1, how: "played" })) || ""));
  check("bad result refused", /result needs/.test((await sa.refused({ t: "result", mid: old.mid, my: -1, their: 1, how: "played" })) || ""));
  check("noshow too early refused", /too early/.test((await sa.refused({ t: "noshow", mid: old.mid })) || ""));
  {
    const m = match(sa.st, 0, 0);
    sa.send({ t: "result", mid: m.mid, code: m.code, my: 3, their: 1, how: "played" });
    sb.report(m, 1, 3);
    await until(() => match(sa.st, 0, 0).status === "done");
    const now = match(sa.st, 0, 0);
    check("semi 0, 3-1 / 1-3: done, side a wins, score [3,1]", now.status === "done" && now.winner === m.a && JSON.stringify(now.score) === "[3,1]", JSON.stringify(now));
    check("winner of semi 0 in the final's slot a", match(sa.st, 1, 0).a === m.a && match(sa.st, 1, 0).status === "waiting");
  }
  check("report on a finished match refused", /over/.test((await sb.refused({ t: "result", mid: r0[0].mid, my: 3, their: 1, how: "played" })) || ""));
  check("report on another team's match refused", /not your match/.test((await sb.refused({ t: "result", mid: r0[1].mid, my: 3, their: 1, how: "played" })) || ""));

  // Semi 1: side a says its opponent left at 1-3, side b says it played 3-1: the scores agree, so
  // the played result stands.
  const ta = byIndex(four, r0[1].a);
  const tb = byIndex(four, r0[1].b);
  {
    const m = match(ta.st, 0, 1);
    ta.report(m, 1, 3, "left");
    tb.report(m, 3, 1, "played");
    await until(() => match(ta.st, 0, 1).status === "done");
    const now = match(ta.st, 0, 1);
    check("semi 1, 'left' 1-3 + 'played' 3-1: done, side b wins, score [1,3]", now.status === "done" && now.winner === m.b && JSON.stringify(now.score) === "[1,3]", JSON.stringify(now));
  }
  const fin = match(ta.st, 1, 0);
  check(
    "final ready: semi winners, fresh code, readyAt",
    fin.status === "ready" && fin.a === r0[0].a && fin.b === r0[1].b && CODE.test(fin.code) && typeof fin.readyAt === "number" && fin.replays === 0,
    JSON.stringify(fin)
  );
  check("final's code is new", fin.code !== r0[0].code && fin.code !== r0[1].code && fin.code !== match(ta.st, 0, 0).code);
  const loser = byIndex(four, r0[0].b);
  check("semi loser cannot report the final", /not your match/.test((await loser.refused({ t: "result", mid: fin.mid, my: 1, their: 0, how: "played" })) || ""));
  const fa = byIndex(four, fin.a);
  const fb = byIndex(four, fin.b);
  fa.report(fin, 2, 0);
  await until(() => match(fb.st, 1, 0).reports.a);
  check("final: one report in, still ready", match(fb.st, 1, 0).status === "ready" && match(fb.st, 1, 0).reports.a?.my === 2);
  fb.report(fin, 0, 2);
  await until(() => four.every((t) => t.st.tourney.champion !== null));
  const end = four[0].st.tourney;
  check("final 2-0 / 0-2: champion = final's side a", end.champion === fin.a && end.rounds[1][0].winner === fin.a && JSON.stringify(end.rounds[1][0].score) === "[2,0]");
  check("finishedAt set, everyone sees the champion", typeof end.finishedAt === "number" && four.every((t) => t.st.tourney.champion === fin.a));
  check("the second socket saw it too", await until(() => b2.state?.tourney?.champion === fin.a));
  check("me.tid still set after the final", four.every((t) => t.st.me.tid === T.tid));
  // Finished: free to join a new lobby, still showing the old result.
  const champ = byIndex(four, fin.a);
  champ.send({ t: "join", size: 4 });
  await until(() => champ.st.me.lobby === 4);
  check("finished team may join a new lobby (old result still shown)", champ.st.me.lobby === 4 && champ.st.tourney?.tid === T.tid);
  champ.send({ t: "leave" });
  await until(() => champ.st.me.lobby === null);
  if (fast) {
    check("after the show time the tournament is gone, me.tid null", await until(() => four.every((t) => t.st.tourney === null && t.st.me.tid === null), 8000));
  } else {
    skip("finished tournament dropped after 30 min");
  }
  for (const t of four) t.ws.close();
  b2.close();
}

// ---------------------------------------------------------------------------------------------
// 3. The timers: a lone report stands, a no-show, a lone draw, "left" wins
// ---------------------------------------------------------------------------------------------

if (fast) {
  const four = await fill(4, ["Echo", "Foxtrot", "Golf", "Hotel"]);
  const r0 = four[0].st.tourney.rounds[0];
  const s0a = byIndex(four, r0[0].a);
  const s1a = byIndex(four, r0[1].a);
  const s1b = byIndex(four, r0[1].b);
  s0a.report(r0[0], 2, 0);
  check("no-show right away refused (too early)", /too early/.test((await s1a.refused({ t: "noshow", mid: r0[1].mid })) || ""));
  await sleep(500);
  check("lone report: still ready after 0.5 s", match(s0a.st, 0, 0).status === "ready");
  await until(() => Date.now() >= match(s1a.st, 0, 1).readyAt + 1100, 3000);
  s1a.send({ t: "noshow", mid: r0[1].mid });
  await until(() => match(s1a.st, 0, 1).status === "done");
  const ns = match(s1a.st, 0, 1);
  check("no-show after the wait: done, claimer wins, score null", ns.status === "done" && ns.winner === r0[1].a && ns.score === null, JSON.stringify(ns));
  check("no-show for a finished match refused", /over/.test((await s1b.refused({ t: "noshow", mid: r0[1].mid })) || ""));
  check("lone report stands after the wait: winner a, [2,0]", await until(() => match(s0a.st, 0, 0).status === "done", 5000) && match(s0a.st, 0, 0).winner === r0[0].a && JSON.stringify(match(s0a.st, 0, 0).score) === "[2,0]", JSON.stringify(match(s0a.st, 0, 0)));
  await until(() => match(s0a.st, 1, 0).status === "ready");
  let fin = match(s0a.st, 1, 0);
  check("final ready", fin.status === "ready" && fin.a === r0[0].a && fin.b === r0[1].a);
  const fa = byIndex(four, fin.a);
  const fb = byIndex(four, fin.b);
  fb.report(fin, 1, 1);
  check("lone draw: replayed after the wait", await until(() => match(fa.st, 1, 0).code !== fin.code, 5000) && match(fa.st, 1, 0).status === "ready" && match(fa.st, 1, 0).replays === 1);
  fin = match(fa.st, 1, 0);
  fb.report(fin, 0, 0, "left");
  await until(() => match(fa.st, 1, 0).reports.b);
  await until(() => Date.now() >= fin.readyAt + 1200, 3000);
  check("no-show refused once the opponent reported", /opponent has reported/.test((await fa.refused({ t: "noshow", mid: fin.mid })) || ""));
  check("lone 'left' stands after the wait: the reporter is champion", await until(() => fa.st.tourney.champion === fin.b, 5000) && match(fa.st, 1, 0).winner === fin.b && JSON.stringify(match(fa.st, 1, 0).score) === "[0,0]");
  for (const t of four) t.ws.close();
} else {
  skip("lone report / no-show / lone draw / 'left' timers");
}

// ---------------------------------------------------------------------------------------------
// 4. Lobby presence: a team gone keeps its place for the grace time, a reconnect keeps it, a
//    silent socket is taken for dead
// ---------------------------------------------------------------------------------------------

if (fast) {
  const p = await team("Papa");
  p.send({ t: "join", size: 4 });
  await until(() => lobbyNames(obs.st, 4).includes("Papa"));
  const gone = Date.now();
  p.ws.close();
  await sleep(500);
  check("closed socket: still in the lobby at first", lobbyNames(obs.st, 4).includes("Papa"));
  await until(() => !lobbyNames(obs.st, 4).includes("Papa"), 5000);
  const took = Date.now() - gone;
  check(`removed after the grace time (${took} ms, grace 1500)`, !lobbyNames(obs.st, 4).includes("Papa") && took >= 1400);

  const q = await team("Quebec");
  q.send({ t: "join", size: 4 });
  await until(() => lobbyNames(obs.st, 4).includes("Quebec"));
  q.ws.close();
  await sleep(300);
  await q.connect();
  await q.hello();
  check("reconnect within the grace: lobby place kept", q.st.me.lobby === 4);
  await sleep(2500);
  check("still in the lobby after the grace time", lobbyNames(obs.st, 4).includes("Quebec") && q.st.me.lobby === 4);
  q.send({ t: "leave" });
  await until(() => !lobbyNames(obs.st, 4).includes("Quebec"));

  const r = await team("Romeo", { ping: false });
  r.send({ t: "join", size: 4 });
  await until(() => lobbyNames(obs.st, 4).includes("Romeo"));
  check("silent lobby socket closed by the server (4001 stale)", await until(() => r.ws.readyState !== WebSocket.OPEN, 8000) && (r.ws.closed === null || /^4001/.test(r.ws.closed)), r.ws.closed);
  check("then out of the lobby after the grace", await until(() => !lobbyNames(obs.st, 4).includes("Romeo"), 5000));
  check("lobby of 4 empty", lobbyNames(obs.st, 4).length === 0);
} else {
  skip("lobby grace, reconnect, silent socket");
}

// ---------------------------------------------------------------------------------------------
// 5. Eight teams; then nobody plays, and the abandoned tournament goes
// ---------------------------------------------------------------------------------------------

{
  const names = ["India", "Juliett", "Kilo", "Lima", "Mike", "November", "Oscar", "Quintet"];
  const eight = await fill(8, names, [-1, 0, 1, 2, 3, 4, 5, 6]);
  const T = eight[0].st.tourney;
  check("observer saw the lobby of 8 at 7 names", obs.ws.got.some((m) => m.t === "state" && lobbyNames(m, 8).length === 7));
  check("8th join starts it for all 8", eight.every((t) => t.st.tourney?.tid === T.tid) && T.size === 8);
  check("rounds of 4, 2, 1", T.rounds.map((r) => r.length).join() === "4,2,1");
  check("4 quarterfinals ready with 4 different codes", T.rounds[0].every((m) => m.status === "ready" && CODE.test(m.code)) && new Set(T.rounds[0].map((m) => m.code)).size === 4);
  check("later rounds waiting", T.rounds.slice(1).flat().every((m) => m.status === "waiting" && m.a === null && m.b === null));
  check("8 names, each team once", T.names.length === 8 && [...T.names].sort().join() === [...names].sort().join() && eight.map((t) => t.st.you).sort().join() === "0,1,2,3,4,5,6,7");
  check("8 stars parallel to names", T.stars?.length === 8 && eight.every((t) => T.stars[t.st.you] === t.star), JSON.stringify(T.stars));
  check("lobby of 8 empty again", await until(() => lobbyNames(obs.st, 8).length === 0));
  for (const t of eight.slice(1)) t.ws.close();
  if (fast) {
    // No report and no result for the idle time (60 min; 8 s in test mode): dropped, teams free.
    check("abandoned tournament dropped after the idle time", await until(() => eight[0].st.tourney === null && eight[0].st.me.tid === null, 12000));
    eight[0].send({ t: "join", size: 8 });
    check("its team may join again", await until(() => eight[0].st.me.lobby === 8));
    eight[0].send({ t: "leave" });
    await until(() => eight[0].st.me.lobby === null);
  } else {
    skip("abandoned tournament dropped after 60 min idle");
  }
  eight[0].ws.close();
}

for (const ws of sockets) {
  try {
    ws.close();
  } catch {}
}
await sleep(300); // let the closes go out
const pass = results.every(Boolean);
console.log(`${results.filter(Boolean).length}/${results.length} checks ok`);
console.log(pass ? "PASS" : "FAIL");
process.exit(pass ? 0 : 1);
