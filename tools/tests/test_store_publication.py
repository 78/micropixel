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
            outputs = []
            for target, machine, elf_flags, arch in [('riscv32-ilp32f', 243, 3, b'riscv32'), ('xtensa', 94, 0, b'xtensa')]:
                aot = root / f'{target}.aot'
                payload = bytearray(64)
                payload[:8] = b'\0aot\x06\0\0\0'
                struct.pack_into('<I', payload, 12, 48)
                struct.pack_into('<HHII', payload, 20, 1, machine, 1, elf_flags)
                payload[48:48 + len(arch)] = arch
                aot.write_bytes(payload)
                output = root / f'{target}.bundle.bin'
                with patch('sys.argv', ['builder', '--app-manifest', str(root/'app.json'), '--aot', str(aot), '--aot-target', target, '--output', str(output)]), redirect_stdout(io.StringIO()):
                    builder.main()
                outputs.append(output)
            args=argparse.Namespace(project=str(root),dry_run=True,output_dir=None,force=False,notes_file=None,tested_device=[])
            with patch.object(CLI,'package_project',side_effect=outputs) as package,patch.object(CLI,'publisher_token') as token,patch.object(CLI,'store_request') as request,redirect_stdout(io.StringIO()) as stdout:
                CLI.run_publish(args)
                token.assert_not_called();request.assert_not_called()
                self.assertEqual([call.args[4:6] for call in package.call_args_list], [('riscv32-ilp32f', False), ('xtensa', False)])
                self.assertEqual([r['target'] for r in json.loads(stdout.getvalue())['releases']], ['riscv32-ilp32f', 'xtensa'])
            args.dry_run = False
            with patch.object(CLI, 'package_project', side_effect=outputs), patch.object(CLI, 'publisher_token', return_value='test'), patch.object(CLI, 'store_request', return_value={}) as request, redirect_stdout(io.StringIO()):
                CLI.run_publish(args)
                self.assertEqual([call.args[2] for call in request.call_args_list[1:]], [p.read_bytes() for p in outputs])
            with patch.object(CLI, 'package_project', side_effect=[outputs[0], CLI.CliError('compile failed')]), patch.object(CLI, 'publisher_token', return_value='test'), patch.object(CLI, 'store_request') as request:
                with self.assertRaisesRegex(CLI.CliError, 'compile failed'):
                    CLI.run_publish(args)
                self.assertEqual(request.call_count, 1)  # Preflight only; neither artifact uploaded.
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
