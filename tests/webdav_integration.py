"""Run the common discovery/copy/resume/multi-server suite over WebDAV."""
import os,runpy
from pathlib import Path
os.environ['ATMOSPHERE_REMOTE_TEST_PROTOCOL']='webdav'
runpy.run_path(str(Path(__file__).with_name('ftp_integration.py')),run_name='__main__')
print('PASS: shared remote-source integration suite over WebDAV')
