"""eq_match.py REF1,REF2,... TARGET OUT: a gentle tone filter (bass and brightness, +-4 dB) matching TARGET to the median spectrum of the reference voices, all reading the same sentences. Needs the voices in /tmp/prodata and a built libRHVoice."""
import sys,os,numpy as np,pyworld as pw,pysptk
REF=sys.argv[1].split(","); TGT=sys.argv[2]; OUT=sys.argv[3]
sys.argv=[sys.argv[0],"/tmp/x",TGT,"/dev/null"]
src=open("/home/claude/rhv/tools/eval_voice.py").read(); sys.path.insert(0,"/home/claude/rhv/scripts")
src=src[:src.index("@njit")].replace('os.path.join(root, "data").encode()','"/tmp/prodata".encode()')
os.environ.setdefault("RHVOICE_LIB","/tmp/rhb2/src/lib/libRHVoice.so"); exec(src)
T=[l.split("\t")[2].strip() for l in open("/home/claude/newv/kareem/corpus2/text.tsv",encoding="utf-8")][1000:1012]
def avg(v):
    global voice; voice=v; M=[]
    for t in T:
        x=synth(t).astype(np.float64)/32768
        f0,tt=pw.harvest(x,24000,frame_period=5); sp=pw.cheaptrick(x,f0,tt,24000)
        mc=pysptk.sp2mc(sp[f0>0],39,0.466); M.append(mc)
    return np.concatenate(M).mean(0)
refs=np.array([avg(v) for v in REF]); ref=np.median(refs,0); tgt=avg(TGT)
eq=ref-tgt; eq[0]=0; eq[9:]=0                      # bass and brightness only (smooth curve)
f=np.linspace(0,12000,257); db=lambda c: 20/np.log(10)*np.real(np.log(pysptk.mc2sp(c,0.466,512)))
curve=db(eq); s=min(1.0,4.0/np.abs(curve).max()); eq*=s
open(OUT+".txt","w").write("dB at 100,300,1k,3k,6k,10k Hz: "+" ".join("%.1f"%db(eq)[int(h/12000*256)] for h in (100,300,1000,3000,6000,10000)))
np.save(OUT,eq.astype(np.float32)); print(open(OUT+".txt").read(), "scale",round(s,2))
