#!/usr/bin/env python3
"""Check receiver health and reject an invalid token without adding test rows."""

import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


class GoogleRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        url = urllib.parse.urlsplit(new_url)
        if (
            url.scheme != 'https'
            or url.hostname not in ('script.google.com', 'script.googleusercontent.com')
            or url.username is not None
            or url.password is not None
            or url.port not in (None, 443)
        ):
            raise ValueError('Unexpected redirect; check that the web app allows device access.')
        return super().redirect_request(request, fp, code, message, headers, new_url)


def main():
    root = Path(__file__).resolve().parents[1]
    settings = (root / 'include/logger_secrets.h').read_text()
    match = re.search(r'LOGGER_SCRIPT_URL\[\]\s*=\s*"([^"]+)"', settings)
    if not match or not re.fullmatch(r'https://script\.google\.com/macros/s/[A-Za-z0-9_-]+/exec', match.group(1)):
        raise ValueError('Set the production /exec URL in include/logger_secrets.h first.')
    endpoint = match.group(1)
    opener = urllib.request.build_opener(GoogleRedirects())

    def request_json(data=None):
        request = urllib.request.Request(endpoint, data=data)
        if data is not None:
            request.add_header('Content-Type', 'application/x-www-form-urlencoded')
        with opener.open(request, timeout=30) as response:
            payload = response.read(4097)
            if len(payload) > 4096:
                raise ValueError('Unexpectedly large response; check the web app deployment.')
            return json.loads(payload)

    health = request_json()
    if not isinstance(health, dict) or health.get('ok') is not True or health.get('service') != 'climate-logger':
        raise ValueError('Receiver health check failed; deploy the current apps-script/Code.gs.')
    print('PASS: receiver health endpoint is reachable without Google sign-in.')

    rejected = request_json(urllib.parse.urlencode({'token': 'invalid'}).encode())
    if not isinstance(rejected, dict) or rejected.get('ok') is not False or rejected.get('error') != 'unauthorized':
        raise ValueError('Receiver did not reject the invalid token as expected.')
    print('PASS: invalid-token request was rejected; no test measurements were submitted.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, urllib.error.URLError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
