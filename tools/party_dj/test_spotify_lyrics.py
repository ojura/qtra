"""Exercise the actual lyric holder with a private DOM/provider/clock in Node."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).with_name('spotify_lyrics.js').resolve()
HARNESS = r'''
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[1], 'utf8');
const target = {uid:'fixture-current',uri:'spotify:track:'+'a'.repeat(22),deviceId:'fixture-device'};
const recording = {artUrl:'https://fixture.invalid/art',durationMs:180000};
const plain = value => JSON.parse(JSON.stringify(value));
const body = (syncType='LINE_SYNCED') => ({lyrics:{syncType,provider:'fixture',providerDisplayName:'Fixture Source',
  lines:[{startTimeMs:'1234',words:'Invented fixture text'},{startTimeMs:'2300',words:''},{startTimeMs:'4500',words:'Another fixture'}]}});
const fixtures = [], checks = [];
async function flush() { for(let i=0;i<24;i++) await Promise.resolve(); }
function fixture(options={}) {
  let clock=1800000000000, timerId=0;
  const timers=new Map(), calls=[], unexpected=[], waits=[];
  const blocked=name=>{unexpected.push(name);throw new Error('Forbidden fixture operation: '+name);};
  const raw={track_window:{current_track:{uid:target.uid,uri:target.uri}}}, device={id:target.deviceId};
  const harmony={_hasFakeState:!!options.fake,getCurrentState:async()=>plain(raw),
    pause:()=>blocked('pause'),resume:()=>blocked('resume'),seek:()=>blocked('seek'),
    nextTrack:()=>blocked('next'),transfer:()=>blocked('transfer')};
  const player={_harmony:harmony,getState:()=>blocked('generic state'),getQueue:()=>blocked('queue'),
    addToQueue:()=>blocked('enqueue')};
  const devices={getActiveDevice:async()=>plain(device)};
  const builder={build:()=>{
    const request={};
    const chain={withHost:value=>{request.host=value;return chain;},withPath:value=>{request.path=value;return chain;},
      withQueryParameters:value=>{request.query=plain(value);return chain;},
      withEndpointIdentifier:value=>{request.endpoint=value;return chain;},send:()=>{
        calls.push(request);
        assert.equal(request.host,'https://spclient.wg.spotify.com/color-lyrics/v2');
        assert.equal(request.path,'/track/'+'a'.repeat(22)+'/image/'+encodeURIComponent(recording.artUrl));
        assert.deepEqual(request.query,{format:'json',vocalRemoval:false});
        assert.equal(request.endpoint,'/track/{trackId}');
        if(options.hold)return new Promise((resolve,reject)=>waits.push({resolve,reject}));
        if(options.error)return Promise.reject(options.error);
        return Promise.resolve({body:options.body||body()});
      }};
    return chain;
  }};
  const builderEntry=options.missing?{get instance(){return blocked('instance getter');}}:{instance:builder};
  const registry={_map:new Map([[Symbol('RequestBuilder'),builderEntry],[Symbol('PlayerAPI'),{instance:player}],
    [Symbol('ConnectDevicesAPI'),{instance:devices}]]),resolve:()=>blocked('registry resolve')};
  const platform={getRegistry:()=>registry,getSession:()=>blocked('getSession')};
  const fiber={memoizedProps:{value:platform}}, element={'__reactFiber$fixture':fiber};
  const window={};
  const sandbox={window,document:{querySelectorAll:selector=>selector==='div'?[element]:blocked('DOM '+selector)},
    Date:class extends Date {static now(){return clock;}},Promise,Map,Set,
    setTimeout:(fn,delay)=>{const id=++timerId;timers.set(id,{fn,at:clock+delay});return id;},
    clearTimeout:id=>timers.delete(id),fetch:()=>blocked('fetch'),WebSocket:()=>blocked('WebSocket'),
    Audio:()=>blocked('Audio'),speechSynthesis:{speak:()=>blocked('speech')},
    location:new Proxy({}, {get:()=>blocked('location read'),set:()=>blocked('navigation')})};
  const context=vm.createContext(sandbox);
  const loaded=vm.runInContext(source,context,{timeout:1000});
  const f={loaded,window,raw,device,harmony,calls,unexpected,waits,context,
    begin:(id='request-a',overrides={})=>window.__laricsLyrics.begin(id,{...target,...overrides},recording,clock+8000),
    poll:(id='request-a')=>plain(window.__laricsLyrics.poll(id)),
    advance:ms=>{clock+=ms;for(const [id,timer] of [...timers])if(timer.at<=clock){timers.delete(id);timer.fn();}},
    resolve:(index=0,value=body())=>waits[index].resolve({body:value})};
  fixtures.push(f);return f;
}
async function check(name,run){await run();checks.push(name);}
(async()=>{
  for(const syncType of ['LINE_SYNCED','SYLLABLE_SYNCED']) await check(syncType,async()=>{
    const f=fixture({body:body(syncType)});assert.equal(f.loaded.ready,true);assert.equal(f.begin().state,'pending');
    await flush();const result=f.poll();assert.equal(result.state,'done');
    assert.equal(result.result.timing,'line');assert.deepEqual(result.result.lines.map(x=>x.startMs),[1234,2300,4500]);
    assert.deepEqual(Object.keys(result.result.lines[0]),['text','startMs']);
    assert.equal(result.result.provider.attribution,'Fixture Source');assert.equal(f.calls.length,1);
  });
  await check('plain source is static',async()=>{
    const f=fixture({body:body('FIXTURE_PLAIN')});f.begin();await flush();const result=f.poll().result;
    assert.equal(result.timing,'none');assert.ok(result.lines.every(line=>line.startMs===null));
  });
  await check('404 is unavailable',async()=>{
    const f=fixture({error:{status:404,message:'PRIVATE FIXTURE CONTENT'}});f.begin();await flush();
    const result=f.poll().result;assert.equal(result.availability,'unavailable');assert.equal(result.reason,'no_lyrics');
    assert.deepEqual(result.lines,[]);
  });
  await check('failure does not expose provider body',async()=>{
    const f=fixture({error:{status:403,message:'PRIVATE FIXTURE CONTENT'}});f.begin();await flush();
    const result=f.poll();assert.equal(result.state,'error');assert.equal(result.result,undefined);
    assert.ok(!JSON.stringify(result).includes('PRIVATE'));
  });
  await check('stale before fetch',async()=>{
    const f=fixture();f.raw.track_window.current_track.uid='new-occurrence';f.begin();await flush();
    assert.equal(f.poll().state,'stale');assert.equal(f.calls.length,0);
  });
  for(const field of ['uid','uri','deviceId'])await check('late '+field,async()=>{
    const f=fixture({hold:true});f.begin();await flush();assert.equal(f.calls.length,1);
    if(field==='deviceId')f.device.id='other-device';
    else f.raw.track_window.current_track[field]=field==='uri'?'spotify:track:'+'b'.repeat(22):'later-duplicate';
    f.resolve();await flush();const result=f.poll();assert.equal(result.state,'stale');assert.equal(result.result,undefined);
  });
  for(const startTimeMs of ['',true,'not-a-clock'])await check('invalid source start '+String(startTimeMs),async()=>{
    const value=body();value.lyrics.lines[0].startTimeMs=startTimeMs;
    const f=fixture({body:value});f.begin();await flush();assert.equal(f.poll().state,'error');
  });
  await check('expiry suppresses late source',async()=>{
    const f=fixture({hold:true});f.begin();await flush();f.advance(8001);assert.equal(f.poll().state,'error');
    f.resolve();await flush();assert.equal(f.poll().state,'error');
  });
  await check('cancel suppresses late source',async()=>{
    const f=fixture({hold:true});f.begin();await flush();f.window.__laricsLyrics.cancel('request-a');
    f.resolve();await flush();assert.equal(f.poll().state,'error');
  });
  await check('independent readers and bounded pending capacity',async()=>{
    const f=fixture({hold:true});const api=f.window.__laricsLyrics;
    for(let i=0;i<8;i++)assert.equal(f.begin('request-'+i).state,'pending');
    assert.equal(f.begin('request-overflow').state,'error');await flush();assert.equal(f.calls.length,8);
    assert.equal(vm.runInContext(source,f.context).ready,true);assert.equal(f.window.__laricsLyrics,api);
    f.resolve(1);f.resolve(0);await flush();assert.equal(f.poll('request-0').state,'done');assert.equal(f.poll('request-1').state,'done');
    for(let i=2;i<8;i++)f.resolve(i);await flush();
    for(let i=2;i<8;i++)assert.equal(f.poll('request-'+i).state,'done');
  });
  await check('missing instance does not resolve or invoke getter',async()=>{
    const f=fixture({missing:true});assert.equal(f.loaded.ready,false);assert.equal(f.calls.length,0);
  });
  await check('synthetic local state blocks read',async()=>{
    const f=fixture({fake:true});f.begin();await flush();assert.equal(f.poll().state,'stale');assert.equal(f.calls.length,0);
  });
  for(const f of fixtures){f.window.__laricsLyrics?.dispose();assert.deepEqual(f.unexpected,[]);}
  console.log(JSON.stringify({cases:checks.length,unexpectedOperations:0}));
})().catch(error=>{console.error(error.stack);process.exitCode=1;});
'''


class SpotifyLyricsHolderTests(unittest.TestCase):
    def test_actual_holder_with_private_provider_and_clock(self):
        candidates = [os.environ.get('QTRA_TEST_NODE'), shutil.which('node'), shutil.which('nodejs'),
                      '/usr/bin/node', '/snap/node/current/bin/node']
        node = None
        for candidate in candidates:
            if not candidate:
                continue
            path = Path(candidate).resolve()
            if path.name not in ('node', 'nodejs'):
                continue
            try:
                with path.open('rb') as stream:
                    executable = stream.read(4) == b'\x7fELF'
                if executable and os.access(path, os.X_OK):
                    node = str(path); break
            except OSError:
                pass
        self.assertIsNotNone(node, 'A direct Node executable is required for the private VM fixture.')
        with tempfile.TemporaryDirectory(prefix='qtra-private-lyrics-') as directory:
            completed = subprocess.run([node, '-e', HARNESS, str(SOURCE)], cwd=directory,
                input='', capture_output=True, text=True, timeout=10,
                env={'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'}, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result, {'cases': 17, 'unexpectedOperations': 0})


if __name__ == '__main__':
    unittest.main()
