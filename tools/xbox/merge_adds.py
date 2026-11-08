# SPDX-License-Identifier: GPL-3.0-or-later
# Merge helper used for the Jadyx -> Lichii port (no merge base): applies only the pure-insertion
# hunks of `diff lichii jadyx`, keeping Lichii-only code. Change hunks are listed with -v for manual merge.
import re,subprocess,sys
# usage: apply_adds.py lichii_file jadyx_file  -> applies pure-insertion hunks (Jadyx adds), reports change/delete hunks
l,j=sys.argv[1],sys.argv[2]
out=subprocess.run(['diff',l,j],capture_output=True,text=True).stdout.splitlines()
lines=open(l).read().split('\n')
hunks=[];i=0
while i<len(out):
    m=re.match(r'^(\d+)(?:,(\d+))?([acd])(\d+)(?:,(\d+))?$',out[i])
    if not m: i+=1; continue
    kind=m.group(3); a1=int(m.group(1)); a2=int(m.group(2) or a1)
    body=[];i+=1
    while i<len(out) and not re.match(r'^\d+(,\d+)?[acd]\d+(,\d+)?$',out[i]):
        body.append(out[i]); i+=1
    hunks.append((kind,a1,a2,body))
applied=0; skipped=[]
for kind,a1,a2,body in reversed(hunks):
    if kind=='a':
        new=[b[2:] for b in body if b.startswith('> ')]
        lines[a1:a1]=new; applied+=1
    else:
        skipped.append((kind,a1,a2,body))
open(l,'w').write('\n'.join(lines))
skipped.reverse()
print(f"{l}: applied {applied} add hunks; {len(skipped)} left (c/d)")
for k,a1,a2,b in skipped:
    if '-v' in sys.argv: print(k,a1,a2,'\n'.join(x[:150] for x in b[:14])); print('----')
