#!/usr/bin/env python3
"""wowee_data_server.py - serves a WoW 3.3.5a client's Data folder in chunks.

The client's "streaming" data mode (pipeline/data_stream.*) reads the
original MPQ archives through a local chunk cache and fetches the chunks it
lacks from here. This serves byte ranges of the files under one folder, plus a
list of those files with their sizes and SHA-256 hashes. Nothing else: no
writes, no directory listings, no files outside the folder.

Access needs a key (these are Blizzard's files): every request carries
"Authorization: Bearer <key>". The key is read from a file at start and is
never part of a URL, and nothing here logs headers - the log lines name the
method, the path, the status and the byte count only.

Protocol (version 1), all GET:
  /v1/files            JSON {"version":1, "chunkSize":N, "files":[{"path":
                       "Data/common.MPQ", "size":S, "mtime":T, "sha256":H}]}.
                       sha256 is null until the background pass has hashed
                       the file (hashes are cached beside the key, keyed by
                       size and mtime, so that happens once).
  /v1/data/<path>      with "Range: bytes=A-B" (one range, at most 32 MiB):
                       206 and those bytes, Content-Range, and
                       X-Content-SHA256 = the SHA-256 of the body, which the
                       client checks before it keeps the chunk.
  401 without the key, 404 for a path not in the list, 416 for a bad range.

Standard library only. Binds to 127.0.0.1; HTTPS comes from the web server
in front of it (a ProxyPass location), so the key never crosses the network
in clear.

  python3 wowee_data_server.py --root <folder> --key-file <file> [--port 8951]
"""
import argparse, hashlib, hmac, http.server, json, os, socketserver, sys, threading, time, urllib.parse

CHUNK = 256 * 1024
MAX_RANGE = 32 * 1024 * 1024


class Store:
    def __init__(self, root, cache_file):
        self.root = os.path.realpath(root)
        self.cache_file = cache_file
        self.lock = threading.Lock()
        self.files = {}      # rel path -> (abs path, size, mtime)
        self.hashes = {}     # rel path -> (size, mtime, sha256)
        self.scan()
        try:
            with open(cache_file) as f:
                for k, v in json.load(f).items():
                    self.hashes[k] = tuple(v)
        except (OSError, ValueError):
            pass

    def scan(self):
        files = {}
        data = os.path.join(self.root, 'Data')
        for dirpath, dirnames, filenames in os.walk(data):
            dirnames.sort()
            for name in sorted(filenames):
                p = os.path.join(dirpath, name)
                if not os.path.isfile(p) or os.path.islink(p):
                    continue
                st = os.stat(p)
                rel = os.path.relpath(p, self.root).replace(os.sep, '/')
                files[rel] = (p, st.st_size, int(st.st_mtime))
        with self.lock:
            self.files = files

    def listing(self):
        with self.lock:
            out = []
            for rel, (p, size, mtime) in sorted(self.files.items()):
                h = self.hashes.get(rel)
                out.append({'path': rel, 'size': size, 'mtime': mtime,
                            'sha256': h[2] if h and h[0] == size and h[1] == mtime else None})
        return {'version': 1, 'chunkSize': CHUNK, 'files': out}

    def lookup(self, rel):
        with self.lock:
            return self.files.get(rel)

    def hash_all(self):
        # Once per file and change, gently: the server has other work.
        for rel, (p, size, mtime) in sorted(self.lookup_all().items()):
            h = self.hashes.get(rel)
            if h and h[0] == size and h[1] == mtime:
                continue
            d = hashlib.sha256()
            try:
                with open(p, 'rb') as f:
                    while True:
                        b = f.read(4 * 1024 * 1024)
                        if not b:
                            break
                        d.update(b)
                        time.sleep(0.01)
            except OSError:
                continue
            with self.lock:
                self.hashes[rel] = (size, mtime, d.hexdigest())
                tmp = self.cache_file + '.tmp'
                with open(tmp, 'w') as f:
                    json.dump(self.hashes, f)
                os.replace(tmp, self.cache_file)
            log('hashed %s' % rel)

    def lookup_all(self):
        with self.lock:
            return dict(self.files)


def log(msg):
    sys.stderr.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
    sys.stderr.flush()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'   # keep-alive: the client asks for many chunks
    server_version = 'wowee-data/1'
    sys_version = ''

    def log_message(self, fmt, *args):
        pass   # never the default line (it holds the request line only, but say so)

    def log_request(self, code='-', size='-'):
        pass

    def _done(self, status, sent):
        path = urllib.parse.urlsplit(self.path).path
        log('%s %s %s %d %d' % (self.client_address[0], self.command, path, status, sent))

    def _send(self, status, body, ctype='text/plain', extra=None):
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
        self._done(status, len(body))

    def _authorised(self):
        auth = self.headers.get('Authorization', '')
        given = auth[7:].strip() if auth.startswith('Bearer ') else ''
        return hmac.compare_digest(given.encode(), self.server.key.encode())

    def do_GET(self):
        if not self._authorised():
            time.sleep(0.5)
            self._send(401, b'unauthorized\n')
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == '/v1/files':
            body = json.dumps(self.server.store.listing()).encode()
            self._send(200, body, 'application/json')
            return
        if not path.startswith('/v1/data/'):
            self._send(404, b'not found\n')
            return
        rel = urllib.parse.unquote(path[len('/v1/data/'):])
        entry = self.server.store.lookup(rel)   # only listed files, so no traversal
        if entry is None:
            self._send(404, b'not found\n')
            return
        p, size, mtime = entry
        rng = self.headers.get('Range', '')
        try:
            unit, spec = rng.split('=', 1)
            a, b = spec.split('-', 1)
            a, b = int(a), int(b)
            if unit.strip() != 'bytes' or a < 0 or b < a or a >= size:
                raise ValueError
            b = min(b, size - 1)
            if b - a + 1 > MAX_RANGE:
                raise ValueError
        except ValueError:
            self._send(416, b'bad range\n', extra={'Content-Range': 'bytes */%d' % size})
            return
        try:
            with open(p, 'rb') as f:
                f.seek(a)
                body = f.read(b - a + 1)
        except OSError:
            self._send(500, b'read failed\n')
            return
        if len(body) != b - a + 1:
            self._send(500, b'short read\n')
            return
        self._send(206, body, 'application/octet-stream', {
            'Content-Range': 'bytes %d-%d/%d' % (a, b, size),
            'X-Content-SHA256': hashlib.sha256(body).hexdigest(),
            'X-File-Mtime': str(mtime),
        })


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True, help='the client folder that holds Data/')
    ap.add_argument('--key-file', required=True)
    ap.add_argument('--port', type=int, default=8951)
    ap.add_argument('--bind', default='127.0.0.1')
    args = ap.parse_args()
    with open(args.key_file) as f:
        key = f.read().strip()
    if len(key) < 32:
        sys.exit('the key in %s is too short' % args.key_file)
    cache = os.path.join(os.path.dirname(os.path.abspath(args.key_file)), 'hashes.json')
    store = Store(args.root, cache)
    srv = Server((args.bind, args.port), Handler)
    srv.key = key
    srv.store = store
    threading.Thread(target=store.hash_all, daemon=True).start()
    log('serving %d files from %s on %s:%d' % (len(store.files), store.root, args.bind, args.port))
    srv.serve_forever()


if __name__ == '__main__':
    main()
