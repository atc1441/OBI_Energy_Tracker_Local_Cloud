# Embedded web pages

Edit the HTML files here. `embed_web.py` creates deterministic gzip arrays in
`.pio/build/<environment>/generated/web_assets.h` before compilation. Generated
files are ignored by Git. No npm tooling or network access is needed.

`sendGzipHtml()` streams each array from flash with its exact binary length and
the gzip response header. The browser performs decompression. Existing route
authentication remains in `gateway_web.cpp`; JSON APIs are not compressed.

Run `python -m pytest tests/test_web_assets.py -q` to check every generated page
through the production response helper, including headers, binary length and
decompression back to its original source bytes.
