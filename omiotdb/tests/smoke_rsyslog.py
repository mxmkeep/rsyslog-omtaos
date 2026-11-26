"""Run against a disposable IoTDB with IOTDB_URL/USER/PASSWORD set.
RSYSLOGD and RSYSLOG_MODULE_DIR select the build containing omiotdb and imudp.
"""
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request
import uuid

base = os.environ['IOTDB_URL'].rstrip('/')
user, password = os.environ['IOTDB_USER'], os.environ['IOTDB_PASSWORD']
db = 'rsyslog_' + uuid.uuid4().hex
command = [os.environ.get('RSYSLOGD', 'rsyslogd'), '-M', os.environ['RSYSLOG_MODULE_DIR']]
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]


def post(operation, database, sql):
    req = urllib.request.Request(base + '/rest/table/v1/' + operation, data=json.dumps({'database': database, 'sql': sql}).encode(),
        headers={'Authorization': 'Basic ' + base64.b64encode((user + ':' + password).encode()).decode(), 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


with tempfile.TemporaryDirectory() as tmp:
    config = Path(tmp) / 'smoke.conf'
    config.write_text(f'''module(load="imudp")
module(load="omiotdb")
input(type="imudp" address="127.0.0.1" port="{port}")
template(name="sample" type="string" string="\\\"%msg%\\\"*:*:INSERT INTO readings(time,device,v) VALUES (1735689600000,'*1*',64)")
if $msg contains 'sample_event' then {{
  action(type="omiotdb" server={json.dumps(base)} db="{db}" table="readings" columns="device STRING TAG,v INT32 FIELD" uid={json.dumps(user)} pwd={json.dumps(password)} template="sample")
}}
''')
    os.chmod(config, 0o600)
    subprocess.run([*command, '-N1', '-f', str(config)], check=True, timeout=10)
    daemon = subprocess.Popen([*command, '-n', '-i', tmp + '/test.pid', '-f', str(config)])
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            for attempt in range(25):
                assert daemon.poll() is None, 'rsyslog exited'
                sock.sendto(b'<14>Jan  1 00:00:00 localhost sensor: sample_event', ('127.0.0.1', port))
                time.sleep(.35)
                result = post('query', db, 'SELECT device,v FROM readings WHERE time=1735689600000 LIMIT 2')
                if result.get('values'):
                    break
        rows = result.get('values', [])
        assert len(rows) == 1 and rows[0][0].strip() == 'sample_event' and rows[0][1] == 64, result
    finally:
        daemon.terminate()
        daemon.wait(timeout=8)
        cleanup = post('nonQuery', '', 'DROP DATABASE IF EXISTS ' + db)
        assert cleanup['code'] == 200, cleanup
    print('PASS rsyslogd -N1, live UDP input -> omiotdb action -> IoTDB table, row read back, random database removed')
