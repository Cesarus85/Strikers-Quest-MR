// Quick match: three fake headsets queue on /quick/T5 and one on /quick/X5. Two of the T5 ones must
// get the same "match <CODE>" and be closed, the third must hear "waiting 1" (and again after a
// ping), and the X5 one must not be matched: another KEY is another queue.
//   node test-quick.mjs [base]      base defaults to ws://127.0.0.1:8787 (npx wrangler dev)
const base = (process.argv[2] || "ws://127.0.0.1:8787").replace(/\/+$/, "");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();

function open(key, name) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${base}/quick/${key}`);
    ws.name = name;
    ws.got = [];
    ws.closed = null;
    ws.onopen = () => resolve(ws);
    ws.onerror = () => reject(new Error(name + ": could not connect to " + base));
    ws.onmessage = (e) => ws.got.push(String(e.data));
    ws.onclose = (e) => (ws.closed = `${e.code} ${e.reason} after ${Date.now() - t0} ms`);
  });
}

const t = [];
for (const name of ["t1", "t2", "t3"]) {
  t.push(await open("T5", name));
  await sleep(200);
}
const x = await open("X5", "x1");
await sleep(1000);

const matchOf = (ws) => (ws.got.find((m) => m.startsWith("match ")) || "").slice(6);
const matched = t.filter(matchOf);
const rest = t.filter((ws) => !matchOf(ws));
const codes = matched.map(matchOf);
// The server's close frame arrives with the match (readyState CLOSING at once); the close event
// itself can come seconds later, when the server side lets the connection go (wrangler dev: ~10 s).
const closing = matched.length === 2 && matched.every((ws) => ws.readyState !== WebSocket.OPEN);

// The one left waiting pings, as the game does every ~5 s, and must get its count back.
let pinged = false;
if (rest.length === 1) {
  const before = rest[0].got.length;
  rest[0].send("ping");
  await sleep(500);
  pinged = rest[0].got.length > before && rest[0].got.at(-1) === "waiting 1";
}
const plain = await fetch(base.replace(/^ws/, "http") + "/quick/T5");

for (let i = 0; i < 150 && matched.some((ws) => ws.closed === null); i++) await sleep(100);

for (const ws of [...t, x]) console.log(`${ws.name}: ${JSON.stringify(ws.got)} close: ${ws.closed}`);
const checks = [
  ["two T5 headsets matched", matched.length === 2],
  ["both got the same code", codes.length === 2 && codes[0] === codes[1]],
  ["code is Q + 5 (no 0/O/1/I)", codes.length > 0 && codes.every((c) => /^Q[A-HJ-NP-Z2-9]{5}$/.test(c))],
  ["matched ones closing at once", closing],
  ["matched ones closed 1000 matched", matched.length === 2 && matched.every((ws) => /^1000 matched /.test(ws.closed))],
  ["third T5 waiting 1, still open", rest.length === 1 && rest[0].got[0] === "waiting 1" && rest[0].closed === null],
  ["ping answered with waiting 1", pinged],
  ["X5 not matched, waiting 1", !matchOf(x) && x.got.at(-1) === "waiting 1" && x.closed === null],
  ["plain GET /quick/T5 is 426", plain.status === 426],
];
for (const [what, ok] of checks) console.log(`${ok ? "ok  " : "BAD "} ${what}`);
for (const ws of [...t, x]) ws.close();
await sleep(300); // let the closes go out, so a quick rerun does not meet this run's leftovers
const pass = checks.every(([, ok]) => ok);
console.log(pass ? "PASS" : "FAIL");
process.exit(pass ? 0 : 1);
