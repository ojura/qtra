"""Exercise audio-act changes on a private display while reading the live monitor.

The test does not change any playback stream or audio route. Its capture client
reads the same monitor as the party visualizer, and its qtra window is private.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from agentctl import AgentClient
from private_display import OwnDisplay, can_draw, child_environment

BUILD = Path(os.environ.get('RUNTIME_AGENT_TEST_BUILD_DIR',ROOT/'build/release'))


@unittest.skipUnless(can_draw(),'a private display is required')
class AudioActs(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(prefix='qtra-audio-acts-')
        self.addCleanup(self.directory.cleanup)
        self.folder=Path(self.directory.name)
        self.display=OwnDisplay()
        self.assertIsNotNone(self.display.start())
        self.addCleanup(self.display.stop)
        env=child_environment(self.display)
        env['XDG_CONFIG_HOME']=str(self.folder/'config')
        self.log=(self.folder/'qtra.log').open('w+')
        self.addCleanup(self.log.close)
        self.process=subprocess.Popen([str(BUILD/'qt_runtime_cube'),'--agent-socket',str(self.folder/'agent.sock')],
                                      env=env,stdout=self.log,stderr=self.log)
        self.addCleanup(self.stop)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            self.assertIsNone(self.process.poll(),'qtra exited during startup')
            self.client=AgentClient(str(self.folder/'agent.sock'),timeout=10)
            try:
                self.client.connect()
                break
            except OSError:
                self.client.close()
                time.sleep(.05)
        else:
            self.fail('qtra socket did not appear')
        self.addCleanup(self.client.close)
        self.module=self.client.request('snippet.load',{'path':str(BUILD/'agent_snippet_audio_pulse.so')})['moduleId']

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(5)

    def invoke(self,request,method='snippet.run',success=True):
        operation=self.client.request(method,{'moduleId':self.module,'executor':'render','request':request})
        result=self.client.wait_for_operation(operation['operationId'],timeout=10)['data']
        self.assertEqual(result['outcome'],'completed' if success else 'failed',result)
        return result.get('result',result)

    def wait_status(self,predicate,seconds=10):
        deadline=time.monotonic()+seconds
        status={}
        while time.monotonic()<deadline:
            status=self.invoke({})
            if predicate(status):return status
            time.sleep(.10)
        self.fail('Audio status did not reach the expected state: '+repr(status))

    def capture(self,name):
        destination=Path(os.environ.get('AUDIO_ACT_CAPTURE_DIR',self.folder))
        destination.mkdir(parents=True,exist_ok=True)
        filename=destination/(name+'.png')
        self.client.request('cube.capture',{'path':str(filename)})
        self.assertTrue(filename.is_file())
        image=Image.open(filename).convert('RGB')
        self.assertGreater(max(ImageStat.Stat(image).stddev),8,'act rendered an empty frame')
        return image

    def write_state(self,filename,value):
        temporary=filename.with_suffix('.new')
        temporary.write_text(json.dumps(value))
        temporary.replace(filename)

    def test_live_capture_style_changes_bounded_history_and_release(self):
        initial=self.invoke({'style':'city','events':True,'device':'@DEFAULT_MONITOR@'})
        self.assertEqual(initial['style'],'city')
        self.assertFalse(initial['circularGeometry'])
        city=self.wait_status(lambda s:s.get('capturing') and s['captureHops']>60 and s['vertices']>1000)
        self.assertEqual(city['cubeScale'],0)
        city_image=self.capture('audio-city-test')
        self.invoke({'style':'waterfall'})
        water=self.wait_status(lambda s:s['style']=='waterfall' and s['captureHops']>city['captureHops']+20
                               and s['historyRows']==s['historyCapacity'])
        self.assertEqual(water['historyCapacity'],96)
        self.assertFalse(water['circularGeometry'])
        self.assertEqual(water['cubeScale'],0)
        self.assertGreater(water['vertices'],20000)
        water_image=self.capture('audio-waterfall-test')
        difference=ImageStat.Stat(ImageChops.difference(city_image,water_image)).mean
        self.assertGreater(max(difference),5,'the two acts should have different compositions')
        state_file=self.folder/'state.json'
        self.write_state(state_file,{'scene':{'audioStyle':'city'}})
        self.invoke({'stateFile':str(state_file)})
        state_city=self.wait_status(lambda s:s['style']=='city' and s['captureHops']>water['captureHops'])
        self.assertEqual(state_city['historyRows'],96)
        self.write_state(state_file,{'scene':{'audioStyle':'unknown'}})
        failed_file=self.wait_status(lambda s:bool(s['styleFileError']))
        self.assertEqual(failed_file['style'],'city')
        self.invoke({'stateFile':'','style':'ring'})
        ring=self.wait_status(lambda s:s['circularGeometry'] and s['captureHops']>state_city['captureHops'] and s['cubeScale']>0)
        self.assertEqual(ring['historyRows'],96)
        self.invoke({'style':'unknown'},success=False)
        self.assertEqual(self.invoke({})['style'],'ring')
        result=self.invoke({},method='snippet.release')
        self.assertTrue(result['removed'])
        self.assertGreater(result['framesDrawn'],30)
        self.assertEqual(self.client.request('object.get',{'objectName':'actionBeatVisualizer','property':'checked'}),False)
        self.log.flush()
        self.log.seek(0)
        self.assertNotIn('QOpenGLShader::',self.log.read())


if __name__=='__main__':
    unittest.main()
