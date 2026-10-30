#!/usr/bin/env python3
import csv, tempfile, subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
exe=root/'editor_thread_benchmark_compare.py'
safe=root/'fixtures/safe.csv';sep=root/'fixtures/separate.csv'
run=lambda a,b: subprocess.run([sys.executable,str(exe),'--safe',str(a),'--separate',str(b)],capture_output=True,text=True)
assert run(safe,sep).returncode==0
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'bad.csv'
 with sep.open(newline='') as f:rows=list(csv.DictReader(f));fields=rows[0].keys()
 rows[0]['effective_mode']='safe'
 with p.open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
 assert run(safe,p).returncode!=0,'false separate must fail'
 rows[0]['effective_mode']='separate';rows[0]['viewport_width']='1920'
 with p.open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
 assert run(safe,p).returncode!=0,'mismatched viewport must fail'
print('PASS benchmark reference positive and two negative probes')