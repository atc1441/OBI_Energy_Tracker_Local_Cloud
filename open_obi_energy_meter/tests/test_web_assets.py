"""Check generated pages through the production sender."""

import importlib.util
import os
from pathlib import Path
import subprocess

import gzip
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('embed_web', ROOT / 'embed_web.py')
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)

# Enough of WebServer for sendGzipHtml(): the collected Accept-Encoding header, the two headers it
# sets, and both send paths. Metadata goes to stdout as
# status / content-type / content-encoding / vary / length; the body goes to a file opened "wb", because
# a body written to stdout would be CRLF-translated on Windows and stop being valid gzip. The request's
# Accept-Encoding arrives in the environment, not in argv, because MinGW expands wildcards in argv and
# would turn a bare "*" into a directory listing.
FAKE_WEBSERVER = r'''#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
using String = std::string;
struct WebServer {
  std::string encoding, vary, accept;
  bool acceptSet = false;
  const char *bodyPath = nullptr;
  WebServer() {
    const char *value = getenv("OBI_TEST_ACCEPT");
    acceptSet = getenv("OBI_TEST_ACCEPT_SET") != nullptr;
    accept = value ? value : "";
  }
  bool hasHeader(const char *name) {
    return std::string(name) == "Accept-Encoding" && acceptSet;
  }
  String header(const char *) { return accept; }
  void sendHeader(const char *name, const char *value) {
    if (std::string(name) == "Content-Encoding") encoding = value;
    if (std::string(name) == "Vary") vary = value;
  }
  void report(int status, const char *type, const char *data, size_t size) {
    // %lu, not %zu: MinGW's format checker rejects %zu and this has to build on Windows too.
    printf("%d\n%s\n%s\n%s\n%lu\n", status, type, encoding.c_str(), vary.c_str(),
           (unsigned long)size);
    FILE *out = fopen(bodyPath, "wb");
    if (!out) return;
    fwrite(data, 1, size, out);
    fclose(out);
  }
  void send(int status, const char *type, const String &data) {
    report(status, type, data.data(), data.size());
  }
  void send_P(int status, const char *type, const char *data, size_t size) {
    report(status, type, data, size);
  }
};
'''


@pytest.fixture(scope='session')
def sender(tmp_path_factory):
    build = tmp_path_factory.mktemp('web-assets')
    embed.generate(ROOT / 'web', build / 'web_assets.h')
    (build / 'pgmspace.h').write_text('#pragma once\n#define PROGMEM\n')
    (build / 'WebServer.h').write_text(FAKE_WEBSERVER)
    source = '#include "web_gzip.h"\n#include "web_assets.h"\n'
    source += 'int main(int argc, char **argv) { if (argc < 3) return 2; WebServer server;\n'
    source += 'server.bodyPath = argv[2];\n'
    for page in sorted((ROOT / 'web').glob('*.html')):
        source += (f'if (std::string(argv[1]) == "{page.stem}") '
                   f'{{ sendGzipHtml(server, {page.stem.upper()}_HTML_GZ); return 0; }}\n')
    source += 'return 3; }\n'
    (build / 'sender.cpp').write_text(source)
    binary = build / 'sender'
    subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(build), '-I', str(ROOT / 'include'),
                    str(build / 'sender.cpp'), '-o', str(binary)], check=True)
    return binary


def serve(sender, page, accept=None):
    """Run the production sender. accept=None means a request with no Accept-Encoding header."""
    body_file = Path(sender).parent / 'body.bin'
    env = {k: v for k, v in os.environ.items()
           if k not in ('OBI_TEST_ACCEPT', 'OBI_TEST_ACCEPT_SET')}
    if accept is not None:
        env['OBI_TEST_ACCEPT_SET'] = '1'
        if accept:                          # Windows cannot hold an empty variable; absent means ""
            env['OBI_TEST_ACCEPT'] = accept
    command = [str(sender), page, str(body_file)]
    meta = subprocess.check_output(command, text=True, env=env).replace('\r', '').split('\n')
    status, mime, encoding, vary, length = meta[0], meta[1], meta[2], meta[3], int(meta[4])
    body = body_file.read_bytes()
    assert vary == 'Accept-Encoding'        # the response varies, so caches have to be told
    assert length == len(body)
    return status, mime, encoding, body


@pytest.mark.parametrize('page', sorted((ROOT / 'web').glob('*.html')), ids=lambda p: p.stem)
def test_served_page(sender, page):
    # No Accept-Encoding at all: RFC 9110 treats any coding as acceptable, so gzip still goes out.
    status, mime, encoding, body = serve(sender, page.stem)
    assert status == '200'
    assert mime == 'text/html; charset=utf-8'
    assert encoding == 'gzip'
    assert b'\x00' in body
    assert gzip.decompress(body) == page.read_bytes()


@pytest.mark.parametrize('accept', [
    'gzip', 'gzip, deflate, br', 'GZIP', 'gzip;q=1.0', '*', 'deflate, *;q=0.5',
    'br;q=1.0, gzip;q=0.2', 'identity;q=0.5, gzip', 'gzip ;q=0.3',
])
def test_gzip_offered(sender, accept):
    status, mime, encoding, body = serve(sender, 'radio', accept)
    assert status == '200'
    assert mime == 'text/html; charset=utf-8'
    assert encoding == 'gzip'
    assert gzip.decompress(body) == (ROOT / 'web/radio.html').read_bytes()


@pytest.mark.parametrize('accept', [
    'identity', 'deflate', 'deflate, br', 'gzip;q=0', 'gzip;q=0.0', '*;q=0', 'gzipped', '',
])
def test_gzip_refused(sender, accept):
    """A client that rules gzip out gets an explanation, never a body it cannot decode."""
    status, mime, encoding, body = serve(sender, 'radio', accept)
    assert status == '406'
    assert mime == 'text/plain'
    assert encoding == ''                   # must not claim a coding the client refused
    assert b'gzip' in body
    assert b'\x00' not in body


def test_incremental_generation(tmp_path):
    pages = tmp_path / 'web'
    pages.mkdir()
    page = pages / 'index.html'
    page.write_text('<html>first</html>')
    header = tmp_path / 'generated.h'
    embed.generate(pages, header)
    before, timestamp = header.read_bytes(), header.stat().st_mtime_ns
    embed.generate(pages, header)
    assert header.read_bytes() == before
    assert header.stat().st_mtime_ns == timestamp
    page.write_text('<html>changed</html>')
    embed.generate(pages, header)
    assert header.read_bytes() != before


def test_missing_pages(tmp_path):
    with pytest.raises(ValueError, match='No web pages'):
        embed.generate(tmp_path, tmp_path / 'generated.h')
