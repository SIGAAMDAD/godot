#!/usr/bin/env python3
"""Offline AB comparison for Godot EDITOR (not remote-game) render threading. Pure stdlib."""
import argparse,csv,statistics,sys,math
from pathlib import Path
KEYS=('build_sha','host_id','backend','viewport_width','viewport_height','vsync','scaling_mode','workload_id','workload_sha256')
REQ=set(KEYS)|{'run_id','trial_id','requested_mode','effective_mode','editor_main_frame_us','frame_id','capture_id','errors'}
def load(p,expected):
 with open(p,newline='',encoding='utf-8') as f:rows=list(csv.DictReader(f))
 if not rows:raise ValueError(f'{p}: empty dataset')
 miss=REQ-set(rows[0]);
 if miss:raise ValueError(f'{p}: missing columns {sorted(miss)}')
 trials={}
 for r in rows:
  if r['effective_mode']!=expected or r['requested_mode']!=expected:raise ValueError(f'{p}: invalid effective/requested mode {r["effective_mode"]}/{r["requested_mode"]}')
  if r['errors'].strip():raise ValueError(f'{p}: frame {r["frame_id"]} reports errors')
  if float(r['editor_main_frame_us'])<=0:raise ValueError(f'{p}: invalid frame duration')
  if 'gpu_supported' in r and r['gpu_supported'] not in ('0','1'):raise ValueError(f'{p}: invalid GPU measurement availability')
  if r.get('gpu_supported')=='0' and r.get('gpu_frame_us') not in ('',None):raise ValueError(f'{p}: unsupported GPU duration must be empty')
  if 'timestamp_us' in r and (not r['timestamp_us'] or int(r['timestamp_us'])<=0):raise ValueError(f'{p}: invalid editor timestamp')
  if r.get('render_queue_wait_us') not in ('',None) and float(r['render_queue_wait_us'])<0:raise ValueError(f'{p}: negative render queue wait')
  if r.get('render_completed_frame_id') not in ('',None) and int(r['render_completed_frame_id'])<0:raise ValueError(f'{p}: invalid completed render frame')
  k=(r['run_id'],r['trial_id'],r['capture_id']);trials.setdefault(k,[]).append(r)
 for t,rr in trials.items():
  seq=[int(x['frame_id']) for x in rr]
  if len(seq)!=len(set(seq)) or seq!=sorted(seq):raise ValueError(f'{p}: duplicate/out-of-order frame in trial {t}')
  if 'timestamp_us' in rr[0]:
   times=[int(x['timestamp_us']) for x in rr]
   if times!=sorted(times) or len(times)!=len(set(times)):raise ValueError(f'{p}: duplicate/out-of-order timestamps in trial {t}')
 return rows,trials
def pctl(a,q):
 s=sorted(a);j=(len(s)-1)*q;lo=int(j);hi=math.ceil(j);return s[lo]+(s[hi]-s[lo])*(j-lo)
def summary(trials,metric):
 vals=[]
 for t,rr in trials.items():
  a=[float(x[metric]) for x in rr if x.get(metric) not in ('',None)]
  if a:vals.append((t,len(a),statistics.median(a),pctl(a,.95),pctl(a,.99)))
 return vals

def low_one_percent(trials):
 rates=[]
 for rr in trials.values():
  durations=sorted((float(r['editor_main_frame_us']) for r in rr),reverse=True)
  slowest=durations[:max(1,math.ceil(len(durations)*0.01))]
  rates.append(1_000_000/statistics.mean(slowest))
 return statistics.median(rates)

def compare(a,b):
 safe,st=load(a,'safe');sep,dt=load(b,'separate')
 for r in safe+sep:
  if r['requested_mode'] not in ('safe','separate'):raise ValueError('bad mode')
 for key in KEYS:
  vals={r[key] for r in safe+sep}
  if len(vals)!=1:raise ValueError(f'mismatched control {key}: {vals}')
 for metric in ('editor_main_frame_us','render_server_frame_us','render_queue_wait_us','gpu_frame_us','input_to_paint_us'):
  if metric not in safe[0] or metric not in sep[0]:continue
  s=summary(st,metric);d=summary(dt,metric)
  if not s or not d:print(f'{metric}: unavailable in a mode');continue
  def agg(a,i):return statistics.median(x[i] for x in a)
  print(f'{metric}: safe p50/p95/p99 {agg(s,2):.2f}/{agg(s,3):.2f}/{agg(s,4):.2f} us; separate {agg(d,2):.2f}/{agg(d,3):.2f}/{agg(d,4):.2f} us; p95 delta {agg(d,3)-agg(s,3):+.2f} us ({100*(agg(d,3)/agg(s,3)-1):+.2f}%)')
 print(f'1% low editor FPS (slowest 1% frame durations): safe {low_one_percent(st):.2f}; separate {low_one_percent(dt):.2f}')
 print(f'Control checks: PASS. Safe trials {len(st)}, separate trials {len(dt)}; safe frames {len(safe)}, separate frames {len(sep)}')
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--safe',required=True);p.add_argument('--separate',required=True);a=p.parse_args()
 try:compare(a.safe,a.separate)
 except (ValueError,KeyError,ZeroDivisionError) as e:print('INVALID COMPARISON:',e,file=sys.stderr);sys.exit(2)