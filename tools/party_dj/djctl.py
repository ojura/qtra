#!/usr/bin/env python3
"""Operate the party service from the DJ's own terminal."""
import argparse
import json
import os
from pathlib import Path
import urllib.error
import urllib.request
import urllib.parse


def main():
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['state','search','events','heartbeat','processing','work','reply','cue','speak','scene','book','request','configure','karaoke','move','reorder','sync','resolve-search','playback','next','pause','resume'])
    p.add_argument('text', nargs='?', default='')
    payload = p.add_mutually_exclusive_group()
    payload.add_argument('--json', default=None)
    payload.add_argument('--file', type=Path, help='Read the JSON request from a UTF-8 file.')
    p.add_argument('--after', type=int, default=0)
    p.add_argument('--wait', type=int, default=0)
    p.add_argument('--url', default='http://127.0.0.1:8787')
    p.add_argument('--data', type=Path, default=Path.home()/'.local/state/larics-dj')
    p.add_argument('--operator', default=os.environ.get('QTRA_DJ_OPERATOR', 'djctl'))
    args = p.parse_args()
    headers = {'Content-Type':'application/json'}
    if args.action == 'state':
        endpoint, data = '/api/state', None
    elif args.action == 'search':
        endpoint, data = '/api/search?' + urllib.parse.urlencode({'q':args.text}), None
    else:
        headers['Authorization'] = 'Bearer ' + (args.data/'admin-token').read_text().strip()
        headers['X-DJ-Operator'] = args.operator
        if args.action == 'events':
            endpoint = '/api/admin/events?' + urllib.parse.urlencode({'after':args.after,'wait':args.wait})
            data = None
        else:
            endpoint = '/api/admin/' + args.action
            body = (json.loads(args.file.read_text(encoding='utf-8')) if args.file else
                    json.loads(args.json) if args.json else {'text':args.text})
            data = json.dumps(body,ensure_ascii=False).encode()
    request = urllib.request.Request(args.url+endpoint,data=data,headers=headers)
    try:
        with urllib.request.urlopen(request,timeout=max(30,args.wait+10)) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        print(error.read().decode('utf-8', errors='replace'))
        raise SystemExit(1)
    print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
