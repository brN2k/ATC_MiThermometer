"""Exercise the actual UTC helper and CLI options without BLE/GUI dependencies."""
import argparse
import ast
import calendar
from contextlib import redirect_stderr
from datetime import datetime, timedelta, timezone
from pathlib import Path
import types
import io
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'python-interface/atc_mi_interface/atc_mi_config.py'
TREE = ast.parse(SOURCE.read_text(encoding='utf-8'))


class ClientTimeTest(unittest.TestCase):
    def helper(self, utc, offset_hours):
        instant = utc.timestamp() + 0.75
        local = utc.astimezone(timezone(timedelta(hours=offset_hours)))
        environment = dict(calendar=calendar, time=types.SimpleNamespace(
            time=lambda: instant, localtime=lambda value: local.timetuple()))
        function = next(node for node in TREE.body if isinstance(node,ast.FunctionDef)
                        and node.name == 'current_device_timestamp')
        exec(compile(ast.Module(body=[function],type_ignores=[]),str(SOURCE),'exec'),environment)
        return environment['current_device_timestamp'], int(instant)

    def parser(self):
        main = next(node for node in TREE.body if isinstance(node,ast.FunctionDef) and node.name == 'main')
        nodes = [node for node in main.body if (
            isinstance(node,ast.Assign) and any(isinstance(target,ast.Name) and target.id == 'date_options'
                                               for target in node.targets)) or (
            isinstance(node,ast.Expr) and isinstance(node.value,ast.Call)
            and isinstance(node.value.func,ast.Attribute) and isinstance(node.value.func.value,ast.Name)
            and node.value.func.value.id == 'date_options')]
        self.assertEqual(len(nodes),3)
        environment = dict(parser=argparse.ArgumentParser())
        exec(compile(ast.Module(body=nodes,type_ignores=[]),str(SOURCE),'exec'),environment)
        return environment['parser']

    def test_utc_and_legacy_in_winter_and_summer(self):
        for month, offset in ((1,1),(7,2)):
            helper, instant = self.helper(datetime(2026,month,15,12,tzinfo=timezone.utc),offset)
            self.assertEqual(helper(use_utc=True),instant)
            self.assertEqual(helper(),instant+offset*3600)
            self.assertEqual(helper(use_utc=False),instant+offset*3600)

    def test_cli_selects_expected_payload(self):
        parser = self.parser()
        self.assertIsNone(parser.parse_args([]).set_date)
        helper, instant = self.helper(datetime(2026,7,15,12,tzinfo=timezone.utc),2)
        for option in ('--set_date_utc','--set-date-utc','--set_date','-D'):
            args = parser.parse_args([option])
            value = helper(use_utc=args.set_date == 'utc')
            payload = bytes([0x23])+value.to_bytes(4,'little')
            expected = instant if 'utc' in option else instant+7200
            self.assertEqual(payload,bytes([0x23])+expected.to_bytes(4,'little'))
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args(['--set_date','--set_date_utc'])


if __name__ == '__main__':
    unittest.main()
