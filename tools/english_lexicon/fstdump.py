import struct, sys
def load(p):
    b=open(p,"rb").read(); o=0
    n,=struct.unpack_from(">H",b,o); o+=2; syms={0:"",1:"?"}
    for i in range(n):
        l=b[o]; o+=1; syms[i+2]=b[o:o+l].decode("utf-8"); o+=l
    ns,=struct.unpack_from(">I",b,o); o+=4; states=[]
    for s in range(ns):
        fin,na=struct.unpack_from(">BI",b,o); o+=5; arcs=[]
        for a in range(na):
            d,i,os_=struct.unpack_from(">IHH",b,o); o+=8; arcs.append((d,i,os_))
        states.append((fin,arcs))
    return syms,states
def paths(syms,states,limit=None):
    out=[]; stack=[(0,"",[])]
    while stack:
        s,inp,outp=stack.pop()
        fin,arcs=states[s]
        if fin: out.append((inp,outp))
        if limit and len(out)>=limit: break
        for d,i,o in arcs: stack.append((d,inp+syms[i],outp+([syms[o]] if syms[o] else [])))
    return out
if __name__=="__main__":
    syms,states=load(sys.argv[1]); print(len(states),"states;",len(syms),"symbols")
    print(sorted(set(syms.values()))[:120])
    P=paths(syms,states,int(sys.argv[2]) if len(sys.argv)>2 else 20)
    for w,ph in P[:15]: print(w, ph)
