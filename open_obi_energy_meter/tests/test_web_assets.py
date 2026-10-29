"""Check generated pages through the production sender."""

import importlib.util
from pathlib import Path
import subprocess

import gzip
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('embed_web', ROOT / 'embed_web.py')
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)


@pytest.fixture(scope='session')
def sender(tmp_path_factory):
    build = tmp_path_factory.mktemp('web-assets')
    embed.generate(ROOT / 'web', build / 'web_assets.h')
    (build / 'pgmspace.h').write_text('#pragma once\n#define PROGMEM\n')
    (build / 'WebServer.h').write_text('''#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
struct WebServer {
  std::string encoding;
  void sendHeader(const char *name, const char *value) {
    if (std::string(name) == "Content-Encoding") encoding = value;
  }
  void send_P(int status, const char *type, const char *data, size_t size) {
    printf("%d\\n%s\\n%s\\n%zu\\n", status, type, encoding.c_str(), size);
    fwrite(data, 1, size, stdout);
  }
};
''')
    source = '#include "web_gzip.h"\n#include "web_assets.h"\n'
    source += 'int main(int argc, char **argv) { if (argc != 2) return 2; WebServer server;\n'
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


@pytest.mark.parametrize('page', sorted((ROOT / 'web').glob('*.html')), ids=lambda p: p.stem)
def test_served_page(sender, page):
    response = subprocess.check_output([str(sender), page.stem])
    status, mime, encoding, length, body = response.split(b'\n', 4)
    assert status == b'200'
    assert mime == b'text/html; charset=utf-8'
    assert encoding == b'gzip'
    assert int(length) == len(body)
    assert b'\x00' in body
    assert gzip.decompress(body) == page.read_bytes()


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
