import argparse
import io
import json
import tempfile
import struct
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch
from tools import build_app_bundle as builder
from tools.tests.test_micropixel_cli import CLI

class PublicationTests(unittest.TestCase):
    def test_store_requests_identify_the_cli(self):
        with patch.object(CLI.urllib.request, 'urlopen', return_value=io.BytesIO(b'{}')) as request:
            CLI.store_request('/api/v1/developer/cli', 'POST', {})
        self.assertEqual(request.call_args.args[0].get_header('User-agent'), f'MicroPixel/{CLI.VERSION}')

    def test_contract_and_rejected_requirements(self):
        fixture=json.loads((Path(__file__).parent/'fixtures/app-requirements-v1.json').read_text())
        self.assertEqual(set(fixture['capabilities']),builder.CAPABILITY_NAMES)
        self.assertEqual(set(fixture['services']),builder.SERVICE_NAMES)
        valid={'schema_version':1,'display':{'layouts':['square'],'min_width':320,'min_height':320},'required':[],'optional':[],'any_of':[['input.touch','input.keys']],'services':{'input':65536}}
        self.assertEqual(builder.validate_requirements(valid),valid)
        for change in ({'required':['unknown']},{'required':['input.touch'],'optional':['input.touch']},{'any_of':[[]]},{'services':{'input':1}},{'schema_version':True}):
            with self.subTest(change=change),self.assertRaises(ValueError):builder.validate_requirements({**valid,**change})

    def test_publish_dry_run_builds_checked_release_without_upload_or_login(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)/'demo'
            with patch('sys.argv',['micropixel','init',str(root)]),redirect_stdout(io.StringIO()):self.assertEqual(CLI.main(),0)
            manifest=json.loads((root/'app.json').read_text())
            aot=root/'demo.aot';payload=bytearray(64);payload[:8]=b'\0aot\x06\0\0\0';struct.pack_into('<I',payload,12,48);struct.pack_into('<HHII',payload,20,1,243,1,3);payload[48:55]=b'riscv32';aot.write_bytes(payload)
            output=root/'demo.bundle.bin'
            with patch('sys.argv',['builder','--app-manifest',str(root/'app.json'),'--aot',str(aot),'--aot-target','riscv32-ilp32f','--output',str(output)]),redirect_stdout(io.StringIO()):builder.main()
            args=argparse.Namespace(project=str(root),dry_run=True,output_dir=None,force=False,notes_file=None,tested_device=[])
            with patch.object(CLI,'package_project',return_value=output) as package,patch.object(CLI,'publisher_token') as token,patch.object(CLI,'store_request') as request,redirect_stdout(io.StringIO()) as stdout:
                CLI.run_publish(args)
                token.assert_not_called();request.assert_not_called()
                self.assertEqual(package.call_args.args[1],'release')
                self.assertEqual(package.call_args.args[4:6],('riscv32-ilp32f',False))
                self.assertIn('"version": "0.1.0"',stdout.getvalue())
            manifest.pop('requirements');(root/'app.json').write_text(json.dumps(manifest))
            with self.assertRaises(CLI.CliError):CLI.run_publish(args)

    def test_publication_rejects_unchecked_or_mislabelled_aot(self):
        for flags, target in [(5,1),(3,1),(1,2)]:
            with self.assertRaises(CLI.CliError):CLI.validate_publication_aot(bytes(64),flags,target)
        with self.assertRaises(CLI.CliError):CLI.validate_publication_aot(b'bad',1,1)

    def test_publisher_credentials_are_origin_bound_and_environment_overridable(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict('os.environ',{'XDG_CONFIG_HOME':directory,'MICROPIXEL_STORE_URL':'https://micropixel.ai','MICROPIXEL_PUBLISH_TOKEN':''}):
            path=CLI.publisher_credentials_path();path.parent.mkdir();path.write_text(json.dumps({'origin':'https://elsewhere.invalid','token':'secret'}))
            with self.assertRaises(CLI.CliError):CLI.publisher_token()
            with patch.dict('os.environ',{'MICROPIXEL_PUBLISH_TOKEN':'ci-token'}):self.assertEqual(CLI.publisher_token(),'ci-token')
            with patch.dict('os.environ',{'MICROPIXEL_STORE_URL':'https://user:secret@host.invalid'}),self.assertRaises(CLI.CliError):CLI.publisher_origin()
