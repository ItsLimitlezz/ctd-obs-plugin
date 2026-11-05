#!/usr/bin/env node
// Spike S2 test server: a stand-in for CTD Relay's file endpoints, in one file with no dependencies (Node 22+).
//
//   node test/s2/server.mjs --dir <storage dir> [--port 8787] [--host 127.0.0.1] [--size-mb 1024] [--token test-token]
//
// GET  /files/<name>   streams the file, with Content-Length and X-Content-SHA256. Needs "Authorization: Bearer <token>".
//                      ?fail=midway   cuts the connection halfway        ?badsha=1   sends a wrong X-Content-SHA256
// PUT  /files/<name>   streams the body to "<name>.<random>.upload", hashing as it goes; on a complete body it fsyncs,
//                      renames it into place and answers 201 with X-Content-SHA256 and {"sha256","size"}. An aborted
//                      or short body deletes the temp file. Auth is checked before "100 Continue", so a refused
//                      upload sends no body. ?fail=end reads the whole body, then answers 500 (as a full disk would).
// GET  /debug/partials lists temp files still in the storage dir (the harness checks it is empty after aborts).
//
// On start it generates /files/big.bin (--size-mb, default 1024) unless it already exists with that size.

import { createHash, randomBytes } from 'node:crypto';
import { createReadStream, createWriteStream, promises as fsp } from 'node:fs';
import { createServer } from 'node:http';
import { join } from 'node:path';

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []),
);
const dir = args.dir;
if (!dir) {
  console.error('usage: server.mjs --dir <storage dir> [--port 8787] [--host 127.0.0.1] [--size-mb 1024]');
  process.exit(2);
}
const port = Number(args.port ?? 8787);
const host = args.host ?? '127.0.0.1';
const sizeMb = Number(args['size-mb'] ?? 1024);
const token = args.token ?? 'test-token';
const hashes = new Map(); // name -> sha256 hex

await fsp.mkdir(dir, { recursive: true });

async function hashFile(path) {
  const h = createHash('sha256');
  for await (const chunk of createReadStream(path, { highWaterMark: 1 << 20 })) h.update(chunk);
  return h.digest('hex');
}

async function generateBig() {
  const path = join(dir, 'big.bin');
  const want = sizeMb * 1024 * 1024;
  const st = await fsp.stat(path).catch(() => null);
  if (st && st.size === want) {
    hashes.set('big.bin', await hashFile(path));
    return;
  }
  const h = createHash('sha256');
  const out = createWriteStream(path);
  // 1 MiB of random bytes, each MiB salted with its index so the file doesn't repeat
  const block = randomBytes(1 << 20);
  for (let i = 0; i < sizeMb; i++) {
    block.writeUInt32LE(i, 0);
    h.update(block);
    if (!out.write(block)) await new Promise((r) => out.once('drain', r));
  }
  await new Promise((r, j) => out.end((e) => (e ? j(e) : r())));
  hashes.set('big.bin', h.digest('hex'));
}

const validName = (n) => /^[A-Za-z0-9._-]{1,128}$/.test(n) && !n.startsWith('.') && !n.endsWith('.upload');
const authorized = (req) => req.headers.authorization === `Bearer ${token}`;

function send(res, code, body, headers = {}) {
  const text = JSON.stringify(body);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text), ...headers });
  res.end(text);
}

async function handleGet(req, res, name, query) {
  const path = join(dir, name);
  const st = await fsp.stat(path).catch(() => null);
  if (!st || !st.isFile()) return send(res, 404, { error: 'not found' });
  let sha = hashes.get(name) ?? (await hashFile(path));
  hashes.set(name, sha);
  if (query.get('badsha')) sha = '0'.repeat(64);
  res.writeHead(200, {
    'Content-Type': 'application/octet-stream',
    'Content-Length': st.size,
    'X-Content-SHA256': sha,
  });
  const stream = createReadStream(path, { highWaterMark: 1 << 20 });
  if (query.get('fail') === 'midway') {
    let sent = 0;
    stream.on('data', (chunk) => {
      sent += chunk.length;
      if (sent >= st.size / 2) {
        stream.destroy();
        req.socket.destroy(); // the client sees a short body
      }
    });
  }
  stream.pipe(res);
}

function handlePut(req, res, name, query) {
  // Content-Length or chunked (FFmpeg's http writer only does chunked); with chunked, only the client's own
  // terminating chunk tells a complete body from a cut one, which Node reports as 'aborted'
  const want = req.headers['content-length'] === undefined ? null : Number(req.headers['content-length']);
  const tmp = join(dir, `${name}.${randomBytes(6).toString('hex')}.upload`);
  const out = createWriteStream(tmp);
  const h = createHash('sha256');
  let got = 0;
  let done = false;
  const discard = () => {
    if (done) return;
    done = true;
    out.destroy();
    fsp.unlink(tmp).catch(() => {});
  };
  out.on('error', discard);
  req.on('data', (chunk) => {
    if (done) return;
    got += chunk.length;
    h.update(chunk);
    if (!out.write(chunk)) {
      req.pause();
      out.once('drain', () => req.resume());
    }
  });
  req.on('aborted', discard);
  req.on('error', discard);
  res.on('close', () => {
    if (!res.writableFinished) discard();
  });
  req.on('end', async () => {
    if (done) return;
    if (want !== null && got !== want) {
      discard();
      return send(res, 400, { error: 'short body' });
    }
    if (query.get('fail') === 'end') {
      discard();
      return send(res, 500, { error: 'injected failure after the body' });
    }
    try {
      await new Promise((r, j) => out.end((e) => (e ? j(e) : r())));
      const fh = await fsp.open(tmp, 'r+');
      await fh.sync();
      await fh.close();
      await fsp.rename(tmp, join(dir, name));
      done = true;
      const sha = h.digest('hex');
      hashes.set(name, sha);
      send(res, 201, { sha256: sha, size: got }, { 'X-Content-SHA256': sha });
    } catch (e) {
      discard();
      send(res, 500, { error: String(e) });
    }
  });
}

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://x');
    if (req.method === 'GET' && url.pathname === '/debug/partials') {
      const names = (await fsp.readdir(dir)).filter((n) => n.endsWith('.upload'));
      return send(res, 200, { partials: names });
    }
    const m = url.pathname.match(/^\/files\/([^/]+)$/);
    if (!m || !validName(m[1])) return send(res, 404, { error: 'not found' });
    if (!authorized(req)) return send(res, 401, { error: 'unauthorized' });
    if (req.method === 'GET') return await handleGet(req, res, m[1], url.searchParams);
    if (req.method === 'PUT') return handlePut(req, res, m[1], url.searchParams);
    send(res, 405, { error: 'method not allowed' });
  } catch (e) {
    if (!res.headersSent) send(res, 500, { error: String(e) });
    else res.destroy();
  }
});

// Expect: 100-continue -> refuse before the body when the token is wrong (what Relay should do too)
server.on('checkContinue', (req, res) => {
  if (!authorized(req)) {
    send(res, 401, { error: 'unauthorized' });
    return;
  }
  res.writeContinue();
  server.emit('request', req, res);
});

const t0 = Date.now();
await generateBig();
server.requestTimeout = 0; // 1 GB bodies take a while on slow links
server.listen(port, host, () => {
  console.log(`s2 server on http://${host}:${port} (big.bin ${sizeMb} MiB sha256 ${hashes.get('big.bin')}, ` +
    `ready in ${((Date.now() - t0) / 1000).toFixed(1)} s)`);
});
