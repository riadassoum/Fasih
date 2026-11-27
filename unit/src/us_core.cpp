/* Fasih unit-selection core (concatenative, non-neural).
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later, like RHVoice. */
#include "us_core.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <cstdlib>
#ifdef FASIH_US_OPUS
#include <opus.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fasih_us
{
  namespace
  {
    std::string join_path(const std::string& a,const std::string& b)
    {
      if(a.empty()) return b;
      char c=a.back();
      return (c=='/'||c=='\\')?a+b:a+"/"+b;
    }

    std::vector<std::string> split_tabs(const std::string& s)
    {
      std::vector<std::string> r; std::string cur;
      for(char c: s){ if(c=='\t'){r.push_back(cur);cur.clear();} else if(c!='\r') cur+=c; }
      r.push_back(cur);
      return r;
    }

    std::vector<std::string> split_spaces(const std::string& s)
    {
      std::vector<std::string> r; std::istringstream is(s); std::string t;
      while(is>>t) r.push_back(t);
      return r;
    }

    uint64_t key3(int p,int h,int n){return (uint64_t(uint32_t(p))<<24)^(uint64_t(h)<<20)^uint64_t(uint32_t(n+1));}

#ifdef _WIN32
    std::wstring widen(const std::string& s)
    {
      int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
      std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
      if(!w.empty()&&w.back()==0) w.pop_back();
      return w;
    }
#endif

    std::ifstream open_in(const std::string& p,std::ios::openmode m=std::ios::in)
    {
#if defined(_WIN32) && defined(_MSC_VER)
      std::ifstream f(widen(p),m);
#else
      std::ifstream f(p,m);
#endif
      if(!f) throw std::runtime_error("fasih_us: cannot open "+p);
      return f;
    }
  }

  voice::voice(const std::string& dir)
  {
    for(int i=0;i<NC_MAX;++i) w.ctx[i]=0;
    load_layout(join_path(dir,"layout.txt"));
    {
      auto f=open_in(join_path(dir,"phones.txt")); std::string l;
      while(std::getline(f,l)){ if(!l.empty()&&l.back()=='\r') l.pop_back(); if(l.empty()) continue; phone_ids[l]=phones.size(); phones.push_back(l);}
    }
    load_context(join_path(dir,"context.txt"));
    load_units(join_path(dir,"units.bin"));
#ifdef FASIH_US_OPUS
    {
      std::ifstream t(join_path(dir,"audio.opa"),std::ios::binary);
      opus=bool(t);
    }
#endif
    load_audio(join_path(dir,opus?"audio.opa":"audio.pcm"));
    try{ load_weights(join_path(dir,"weights.txt")); } catch(...) {}
    try{ load_marks(join_path(dir,"marks.bin")); } catch(...) {mark_off.clear();}
    build_index();
  }

  voice::~voice()
  {
#ifdef _WIN32
    if(pcm && map_handle){ UnmapViewOfFile(pcm); CloseHandle((HANDLE)map_handle); CloseHandle((HANDLE)file_handle);}
#else
    if(pcm && map_size){ munmap((void*)pcm,map_size); }
#endif
  }

  void voice::load_layout(const std::string& path)
  {
    auto f=open_in(path); std::string l;
    while(std::getline(f,l))
      {
        if(!l.empty()&&l.back()=='\r') l.pop_back();
        if(l.empty()) continue;
        auto p=split_tabs(l);
        while(p.size()<3) p.push_back("");
        layout.push_back({p[0],p[1],p[2]});
      }
    for(size_t i=0;i<layout.size();++i)
      {
        if(layout[i].name=="name") f_name=i;
        else if(layout[i].name=="prev_name") f_prev=i;
        else if(layout[i].name=="next_name") f_next=i;
      }
    if(f_name<0) throw std::runtime_error("fasih_us: layout without name");
  }

  void voice::load_context(const std::string& path)
  {
    auto f=open_in(path); std::string l;
    while(std::getline(f,l))
      {
        if(!l.empty()&&l.back()=='\r') l.pop_back();
        if(l.empty()) continue;
        auto p=split_tabs(l);
        while(p.size()<3) p.push_back("");
        ctxdef d; d.name=p[0]; d.clip=p[1].empty()?-1:std::stoi(p[1]); d.field=-1;
        auto vals=split_spaces(p[2]);
        for(size_t i=0;i<vals.size();++i) d.vals[vals[i]]=i;
        for(size_t i=0;i<layout.size();++i) if(layout[i].name==d.name) d.field=i;
        ctxdefs.push_back(d);
      }
    nc=ctxdefs.size();
    if(nc>NC_MAX) throw std::runtime_error("fasih_us: too many context features");
    // default target weights
    for(int k=0;k<nc;++k)
      {
        const std::string& n=ctxdefs[k].name;
        double v=0.3;
        if(n=="prev_name"||n=="next_name") v=2.0;
        else if(n=="prev_prev_name"||n=="next_next_name") v=0.5;
        else if(n=="syl_stress") v=1.5;
        else if(n=="syl_pos_in_phrase_bw") v=1.5;
        else if(n=="word_pos_in_phrase_bw"||n=="phrase_pos_in_utt_bw") v=1.0;
        else if(n=="syl_pos_in_phrase_fw"||n=="syl_pos_in_word_bw") v=0.6;
        else if(n=="next_syl_stress"||n=="pos_in_word_bw") v=0.5;
        else if(n=="pos_in_syl_fw"||n=="pos_in_syl_bw") v=0.4;
        else if(n=="utt_type") v=1.5;
        w.ctx[k]=v;
      }
  }

  void voice::load_weights(const std::string& path)
  {
    auto f=open_in(path); std::string l;
    while(std::getline(f,l))
      {
        auto p=split_spaces(l);
        if(p.size()<2||p[0][0]=='#') continue;
        double v=std::stod(p[1]);
        bool done=false;
        for(int k=0;k<nc;++k) if(ctxdefs[k].name==p[0]){w.ctx[k]=v;done=true;}
        if(done) continue;
        if(p[0]=="join_spec") w.join_spec=v;
        else if(p[0]=="join_f0") w.join_f0=v;
        else if(p[0]=="join_energy") w.join_energy=v;
        else if(p[0]=="join_vuv") w.join_vuv=v;
        else if(p[0]=="join_base_mid") w.join_base_mid=v;
        else if(p[0]=="join_base_edge") w.join_base_edge=v;
        else if(p[0]=="target") w.target=v;
        else if(p[0]=="join") w.join=v;
        else if(p[0]=="max_cands") w.max_cands=int(v);
      }
  }

  void voice::load_units(const std::string& path)
  {
    auto f=open_in(path,std::ios::binary);
    char magic[8]; f.read(magic,8);
    if(std::memcmp(magic,"FSUV0001",8)!=0) throw std::runtime_error("fasih_us: bad units.bin");
    uint32_t hdr[5]; f.read((char*)hdr,sizeof(hdr));
    uint32_t nu=hdr[0], nutt=hdr[1]; jd=hdr[2]; uint32_t nc_file=hdr[3]; sr=hdr[4];
    if(jd>JD_MAX||int(nc_file)!=nc) throw std::runtime_error("fasih_us: units.bin does not match context.txt");
    std::vector<float> sd(jd); f.read((char*)sd.data(),4*jd);
    utts.resize(nutt);
    for(auto& x: utts){ uint64_t b; uint32_t n; f.read((char*)&b,8); f.read((char*)&n,4); x={b,n}; }
    u.resize(nu);
    for(auto& x: u)
      {
        uint16_t p; uint8_t h,pad; uint32_t ut; uint64_t a,b;
        f.read((char*)&p,2); f.read((char*)&h,1); f.read((char*)&pad,1); f.read((char*)&ut,4); f.read((char*)&a,8); f.read((char*)&b,8);
        x.phone=p; x.half=h; x.utt=ut; x.start=a; x.end=b;
        std::memset(x.ctx,0,sizeof(x.ctx));
        f.read((char*)x.ctx,nc);
        f.read((char*)x.jl,4*jd); f.read((char*)&x.lf0l,4);
        f.read((char*)x.jr,4*jd); f.read((char*)&x.lf0r,4);
      }
    if(!f) throw std::runtime_error("fasih_us: truncated units.bin");
  }

  void voice::load_audio(const std::string& path)
  {
#ifdef _WIN32
    HANDLE fh=CreateFileW(widen(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);
    if(fh==INVALID_HANDLE_VALUE) throw std::runtime_error("fasih_us: cannot open audio");
    LARGE_INTEGER sz; GetFileSizeEx(fh,&sz);
    HANDLE mh=CreateFileMappingW(fh,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!mh){CloseHandle(fh); throw std::runtime_error("fasih_us: cannot map audio");}
    pcm=(const int16_t*)MapViewOfFile(mh,FILE_MAP_READ,0,0,0);
    if(!pcm){CloseHandle(mh);CloseHandle(fh); throw std::runtime_error("fasih_us: cannot map audio");}
    file_handle=fh; map_handle=mh; map_size=size_t(sz.QuadPart);
#else
    int fd=::open(path.c_str(),O_RDONLY);
    if(fd<0) throw std::runtime_error("fasih_us: cannot open audio");
    struct stat st; fstat(fd,&st);
    void* p=mmap(nullptr,st.st_size,PROT_READ,MAP_SHARED,fd,0);
    ::close(fd);
    if(p==MAP_FAILED) throw std::runtime_error("fasih_us: cannot map audio");
    pcm=(const int16_t*)p; map_size=st.st_size;
#endif
    if(!opus){ n_samples=map_size/2; return; }
    opa=(const unsigned char*)pcm; pcm=nullptr;
    if(map_size<24||std::memcmp(opa,"FSOP0001",8)!=0) throw std::runtime_error("fasih_us: bad audio.opa");
    uint32_t h[4]; std::memcpy(h,opa+8,16);
    if(h[0]!=utts.size()||int(h[1])!=sr) throw std::runtime_error("fasih_us: audio.opa does not match units.bin");
    opa_frame=h[2]; opa_la=h[3];
    opa_utts.resize(h[0]);
    for(uint32_t i=0;i<h[0];++i){ std::memcpy(&opa_utts[i].off,opa+24+16*i,8); std::memcpy(&opa_utts[i].frames,opa+32+16*i,4); std::memcpy(&opa_utts[i].samples,opa+36+16*i,4);}
    for(auto& x: utts) n_samples=std::max<uint64_t>(n_samples,x.first+x.second);
  }

  void voice::read(uint32_t utt,uint64_t a,uint64_t b,float* out) const
  {
    if(b<=a) return;
    if(!opus)
      {
        for(uint64_t p=a;p<b;++p) out[p-a]=sample(p)/32768.0f;
        return;
      }
#ifdef FASIH_US_OPUS
    const uint64_t base=utts[utt].first;
    const opa_utt& U=opa_utts[utt];
    const int64_t ra=int64_t(a-base)+opa_la, rb=int64_t(b-base)+opa_la;   // positions in the decoded stream
    const int FR=opa_frame;
    const int PRE=4;                                                       // 80 ms of decoder pre-roll
    int64_t fa=ra/FR-PRE; if(fa<0) fa=0;
    int64_t fb=(rb+FR-1)/FR; if(fb>int64_t(U.frames)) fb=U.frames;
    int err=0;
    OpusDecoder* dec=opus_decoder_create(sr,1,&err);
    if(err!=OPUS_OK) throw std::runtime_error("fasih_us: opus decoder");
    const uint32_t* offs=(const uint32_t*)(opa+U.off);
    const unsigned char* data=opa+U.off+4*(size_t(U.frames)+1);
    std::vector<float> fr(FR);
    std::fill(out,out+(b-a),0.0f);
    for(int64_t f=fa;f<fb;++f)
      {
        uint32_t o0,o1; std::memcpy(&o0,offs+f,4); std::memcpy(&o1,offs+f+1,4);
        int n=opus_decode_float(dec,data+o0,int(o1-o0),fr.data(),FR,0);
        if(n<=0) continue;
        int64_t p0=f*FR;
        for(int j=0;j<n;++j)
          {
            int64_t q=p0+j;
            if(q>=ra&&q<rb) out[q-ra]=fr[j];
          }
      }
    opus_decoder_destroy(dec);
#endif
  }

  void voice::load_marks(const std::string& path)
  {
    auto f=open_in(path,std::ios::binary);
    char magic[8]; f.read(magic,8);
    if(std::memcmp(magic,"FSPM0001",8)!=0) throw std::runtime_error("bad marks");
    uint32_t n; f.read((char*)&n,4);
    if(n!=utts.size()) throw std::runtime_error("marks do not match");
    mark_off.assign(1,0);
    for(uint32_t i=0;i<n;++i)
      {
        uint32_t c; f.read((char*)&c,4);
        size_t o=mark_data.size(); mark_data.resize(o+c);
        f.read((char*)(mark_data.data()+o),4*size_t(c));
        mark_off.push_back(mark_data.size());
      }
    if(!f) throw std::runtime_error("truncated marks");
  }

  void voice::build_index()
  {
    for(uint32_t i=0;i<u.size();++i)
      {
        const unit& x=u[i];
        // neighbour phone for the half: left half -> prev phone, right half -> next phone
        int kfeat=-1;
        for(int k=0;k<nc;++k)
          if(ctxdefs[k].name==(x.half==0?"prev_name":"next_name")) kfeat=k;
        int nb=-1;
        if(kfeat>=0)
          {
            // ctx holds the vocabulary id; map back to the phone id
            for(auto& kv: ctxdefs[kfeat].vals) if(kv.second==x.ctx[kfeat]){ auto it=phone_ids.find(kv.first); nb=(it==phone_ids.end())?-1:it->second; break;}
          }
        index[key3(x.phone,x.half,nb)].push_back(i);
        auto& wide=index_wide[key3(x.phone,x.half,-2)];
        wide.push_back(i);
      }
    // keep the wide lists short: an even sample
    for(auto& kv: index_wide)
      {
        auto& v=kv.second;
        if(v.size()>400)
          {
            std::vector<uint32_t> s; double step=double(v.size())/400.0;
            for(int i=0;i<400;++i) s.push_back(v[size_t(i*step)]);
            v.swap(s);
          }
      }
  }

  int voice::phone_id(const std::string& p) const
  {
    auto it=phone_ids.find(p);
    return it==phone_ids.end()?-1:it->second;
  }

  const std::vector<uint32_t>& voice::candidates(int phone,int half,int neighbour) const
  {
    auto it=index.find(key3(phone,half,neighbour));
    if(it!=index.end()&&it->second.size()>=8) return it->second;
    auto jt=index_wide.find(key3(phone,half,-2));
    if(it!=index.end()&&!it->second.empty()&&(jt==index_wide.end())) return it->second;
    return jt==index_wide.end()?empty:jt->second;
  }

  const int16_t* voice::samples(uint64_t pos) const {return pcm+pos;}
  int16_t voice::sample(uint64_t pos) const {return pos<n_samples?pcm[pos]:0;}

  bool voice::parse_label(const std::string& lab,char utt_type,target& left,target& right) const
  {
    std::vector<std::string> v(layout.size());
    size_t pos=0;
    for(size_t i=0;i<layout.size();++i)
      {
        const field& fd=layout[i];
        if(i==0&&!fd.left.empty()&&lab.compare(0,fd.left.size(),fd.left)==0) pos+=fd.left.size();
        if(fd.right.empty()){ v[i]=lab.substr(std::min(pos,lab.size())); pos=lab.size(); continue;}
        size_t e=lab.find(fd.right,pos);
        if(e==std::string::npos){ v[i]=lab.substr(std::min(pos,lab.size())); pos=lab.size(); continue;}
        v[i]=lab.substr(pos,e-pos); pos=e+fd.right.size();
      }
    const std::string& name=v[f_name];
    left=target(); right=target();
    if(name=="pau"||phone_id(name)<0)
      {
        left.phone=-1; left.pause_ms=0;
        return false;
      }
    for(int h=0;h<2;++h)
      {
        target& t=(h==0)?left:right;
        t.phone=phone_id(name); t.half=h; t.pause_ms=0;
        int nf=(h==0)?f_prev:f_next;
        t.neighbour=(nf>=0)?phone_id(v[nf]):-1;
        for(int k=0;k<nc;++k)
          {
            const ctxdef& d=ctxdefs[k];
            t.known[k]=true;
            if(d.name=="utt_type"){ t.ctx[k]=(utt_type=='q')?1:(utt_type=='e')?2:0; continue;}
            if(d.field<0){t.known[k]=false;t.ctx[k]=0;continue;}
            const std::string& s=v[d.field];
            if(d.clip>=0)
              {
                char* endp=nullptr; long x=std::strtol(s.c_str(),&endp,10);
                if(endp==s.c_str()) x=0;
                t.ctx[k]=uint8_t(std::max(0L,std::min<long>(x,d.clip)));
              }
            else
              {
                auto it=d.vals.find(s);
                if(it==d.vals.end()){t.known[k]=false;t.ctx[k]=255;}
                else t.ctx[k]=uint8_t(it->second);
              }
          }
      }
    return true;
  }

  namespace
  {
    double target_cost(const voice& v,const target& t,const unit& x)
    {
      const weights& w=v.get_weights();
      double c=0;
      int nc=v.nctx();
      for(int k=0;k<nc;++k)
        {
          if(w.ctx[k]==0) continue;
          if(!t.known[k]||t.ctx[k]!=x.ctx[k]) c+=w.ctx[k];
        }
      return c;
    }

    double join_cost(const voice& v,uint32_t a,uint32_t b)
    {
      if(b==a+1) return 0;
      const auto& U=v.units();
      const unit& x=U[a]; const unit& y=U[b];
      const weights& w=v.get_weights();
      double d=0;
      { double e=(x.jr[0]-y.jl[0])*w.join_energy; d+=e*e; }
      for(int i=1;i<v.jdim();++i){ double e=x.jr[i]-y.jl[i]; d+=e*e;}
      double c=w.join_spec*std::sqrt(d/v.jdim());
      bool va=x.lf0r>0, vb=y.lf0l>0;
      if(va&&vb) c+=w.join_f0*std::fabs(x.lf0r-y.lf0l)*10.0;
      else if(va!=vb) c+=w.join_vuv;
      c+=(y.half==1)?w.join_base_mid:w.join_base_edge;
      return c;
    }
  }

  select_result select_units(const voice& v,const std::vector<target>& T)
  {
    const auto& U=v.units();
    const weights& w=v.get_weights();
    select_result res;
    size_t n=T.size();
    std::vector<std::vector<uint32_t>> cand(n);
    std::vector<std::vector<double>> cost(n);
    std::vector<std::vector<int>> back(n);
    const double INF=std::numeric_limits<double>::infinity();
    for(size_t i=0;i<n;++i)
      {
        const target& t=T[i];
        if(t.phone<0) continue;
        const auto& pool=v.candidates(t.phone,t.half,t.neighbour);
        std::vector<std::pair<double,uint32_t>> scored;
        scored.reserve(pool.size()+64);
        for(uint32_t id: pool) scored.push_back({target_cost(v,t,U[id]),id});
        size_t K=std::min<size_t>(w.max_cands,scored.size());
        std::partial_sort(scored.begin(),scored.begin()+K,scored.end());
        scored.resize(K);
        // continuations of the previous step's candidates keep natural runs available
        if(i>0&&T[i-1].phone>=0)
          for(uint32_t p: cand[i-1])
            {
              uint32_t q=p+1;
              if(q<U.size()&&U[q].utt==U[p].utt&&U[q].phone==uint32_t(t.phone)&&U[q].half==t.half)
                scored.push_back({target_cost(v,t,U[q]),q});
            }
        std::sort(scored.begin(),scored.end(),[](auto& a,auto& b){return a.second<b.second;});
        scored.erase(std::unique(scored.begin(),scored.end(),[](auto& a,auto& b){return a.second==b.second;}),scored.end());
        if(scored.empty())
          {
            // nothing at all for this phone: fall back to any unit of the same half
            for(uint32_t id=0;id<U.size()&&scored.size()<1;++id) if(U[id].half==t.half) scored.push_back({100.0,id});
          }
        for(auto& s: scored){cand[i].push_back(s.second); cost[i].push_back(s.first*w.target);}
        back[i].assign(cand[i].size(),-1);
      }
    // Viterbi
    int prev=-1;
    for(size_t i=0;i<n;++i)
      {
        if(T[i].phone<0) continue;
        if(prev>=0&&size_t(prev)==i-1)
          {
            std::vector<double> nc(cand[i].size(),INF);
            for(size_t b=0;b<cand[i].size();++b)
              {
                double best=INF; int arg=-1;
                for(size_t a=0;a<cand[prev].size();++a)
                  {
                    double c=cost[prev][a]+w.join*join_cost(v,cand[prev][a],cand[i][b]);
                    if(c<best){best=c;arg=a;}
                  }
                nc[b]=best+cost[i][b]; back[i][b]=arg;
              }
            cost[i].swap(nc);
          }
        else if(prev>=0)
          {
            // after a pause: start from the best path so far
            double best=*std::min_element(cost[prev].begin(),cost[prev].end());
            for(auto& c: cost[i]) c+=best;
          }
        prev=i;
      }
    res.units.assign(n,-1);
    res.pause_ms.assign(n,0);
    for(size_t i=0;i<n;++i) if(T[i].phone<0) res.pause_ms[i]=T[i].pause_ms;
    // backtrack, segment by segment
    int i=int(n)-1;
    while(i>=0)
      {
        if(T[i].phone<0){--i;continue;}
        int b=std::min_element(cost[i].begin(),cost[i].end())-cost[i].begin();
        if(i==int(n)-1||T[i+1].phone<0) res.cost+=cost[i][b];
        while(i>=0&&T[i].phone>=0)
          {
            res.units[i]=cand[i][b];
            int a=back[i][b];
            --i;
            if(a<0) break;
            b=a;
          }
      }
    return res;
  }

  namespace
  {
    // A stretch of one recording, copied into a buffer with margins on both sides,
    // whose pitch may be bent near its edges to meet its neighbours.
    struct piece
    {
      uint64_t a,b;          // the selected range (global samples)
      uint64_t A;            // global position of buf[0]
      uint32_t utt;
      bool after_pause;
      double pause;
      double f0a,f0b;        // Hz at the edges (0 = unvoiced)
      double ga{1},gb{1};    // pitch scale wanted at the start / end
      std::vector<float> buf;
      float at(uint64_t p) const
      {
        if(p<A||p>=A+buf.size()) return 0;
        return buf[p-A];
      }
    };

    // TD-PSOLA over buf[z0,z1) (local indices) with a pitch scale curve sc(local index).
    template<class F> void psola(const voice& v,piece& s,int64_t z0,int64_t z1,F sc)
    {
      if(z1<=z0) return;
      const int sr=v.sample_rate();
      uint32_t nm; const uint32_t* M=v.marks(s.utt,nm);
      if(!M||nm<3) return;
      const int64_t ubase=int64_t(v.utt_start(s.utt))-int64_t(s.A); // utterance start, local
      std::vector<int64_t> mk; std::vector<char> vo;
      for(uint32_t i=0;i<nm;++i)
        {
          int64_t p=ubase+int64_t(M[i]>>1);
          if(p<z0-sr/40||p>z1+sr/40) continue;
          mk.push_back(p); vo.push_back(M[i]&1);
        }
      if(mk.size()<3) return;
      const int64_t n=z1-z0;
      std::vector<float> o(n,0.0f), ws(n,0.0f);
      const int UV=sr/200;
      auto period=[&](size_t k)->int{
        int64_t d=0;
        if(k+1<mk.size()&&vo[k+1]) d=mk[k+1]-mk[k];
        else if(k>0&&vo[k-1]) d=mk[k]-mk[k-1];
        if(d<sr/500||d>sr/50) d=UV;
        return int(d);
      };
      double t=double(mk[0]);
      size_t k=0;
      const int64_t L=int64_t(s.buf.size());
      while(t<z1+0.0)
        {
          while(k+1<mk.size()&&std::fabs(mk[k+1]-t)<=std::fabs(mk[k]-t)) ++k;
          bool voiced=vo[k];
          int P=voiced?period(k):UV;
          int64_t src=voiced?mk[k]:int64_t(t);
          int64_t dst=int64_t(std::lround(t));
          for(int j=-P;j<P;++j)
            {
              int64_t si=src+j, di=dst+j;
              if(si<0||si>=L||di<z0||di>=z1) continue;
              float w=float(0.5+0.5*std::cos(3.14159265358979323846*j/P));
              o[di-z0]+=w*s.buf[si]; ws[di-z0]+=w;
            }
          double g=voiced?sc(dst):1.0;
          if(g<0.5) g=0.5; if(g>2.0) g=2.0;
          t+=P/g;
        }
      // write back, blending with the original where the window sum is weak and at both ends
      const int X=std::min<int64_t>(sr/200,n/4);
      for(int64_t i=0;i<n;++i)
        {
          float raw=s.buf[z0+i];
          float y=(ws[i]>0.3f)?o[i]/ws[i]:raw;
          float bl=1.0f;
          if(i<X&&z0>0) bl=float(i)/X;          // fade in from the original
          if(n-1-i<X&&z1<L) bl=std::min(bl,float(n-1-i)/X);
          s.buf[z0+i]=bl*y+(1-bl)*raw;
        }
    }
  }

  std::vector<size_t>* debug_joins=nullptr;
  void concatenate(const voice& v,const select_result& sel,std::vector<float>& out,double pause_scale)
  {
    const auto& U=v.units();
    const int sr=v.sample_rate();
    const int L=sr/100;          // 10 ms cross-fade
    const int M=sr/200;          // +-5 ms search for the best match
    const int F=sr/400;          // 2.5 ms fades at pauses
    const int R=sr*45/1000;      // pitch ramp length on each side of a join
    const int PAD=L+M+2*sr/100;
    std::vector<piece> segs;
    double pending_pause=0; bool pause_flag=true;
    for(size_t i=0;i<sel.units.size();++i)
      {
        int64_t id=sel.units[i];
        if(id<0){pending_pause+=sel.pause_ms[i]; pause_flag=true; continue;}
        const unit& x=U[id];
        if(!segs.empty()&&!pause_flag&&i>0&&sel.units[i-1]==id-1) {segs.back().b=x.end; segs.back().f0b=x.lf0r>0?std::exp(x.lf0r):0; continue;}
        piece p; p.a=x.start; p.b=x.end; p.utt=x.utt; p.after_pause=pause_flag; p.pause=pending_pause;
        p.f0a=x.lf0l>0?std::exp(x.lf0l):0; p.f0b=x.lf0r>0?std::exp(x.lf0r):0; p.A=0;
        segs.push_back(p);
        pending_pause=0; pause_flag=false;
      }
    double tail_pause=pending_pause;
    // pitch meeting points at the joins
    for(size_t i=1;i<segs.size();++i)
      {
        piece& p=segs[i-1]; piece& q=segs[i];
        if(q.after_pause||p.f0b<=0||q.f0a<=0) continue;
        double r=std::log(q.f0a/p.f0b);
        if(std::fabs(r)<0.015||std::fabs(r)>0.45) continue;   // tiny, or an octave error
        double m=std::sqrt(p.f0b*q.f0a);
        p.gb=m/p.f0b; q.ga=m/q.f0a;
      }
    // fill the buffers and bend the pitch near the joins
    for(piece& s: segs)
      {
        uint64_t us=v.utt_start(s.utt), ue=v.utt_end(s.utt);
        s.A=(s.a>us+PAD)?s.a-PAD:us;
        uint64_t B=std::min<uint64_t>(ue,s.b+PAD);
        s.buf.resize(B-s.A);
        v.read(s.utt,s.A,B,s.buf.data());
        if(!v.has_marks()) continue;
        int64_t la=int64_t(s.a-s.A), lb=int64_t(s.b-s.A), n=int64_t(s.buf.size());
        int64_t r=std::min<int64_t>(R,(lb-la)/2);
        if(r<sr/200) continue;
        if(s.ga!=1.0)
          {
            double g=s.ga;
            psola(v,s,0,std::min(n,la+r+sr/200),[&](int64_t i){ if(i<=la) return g; if(i>=la+r) return 1.0; return std::pow(g,double(la+r-i)/r);});
          }
        if(s.gb!=1.0)
          {
            double g=s.gb;
            psola(v,s,std::max<int64_t>(0,lb-r-sr/200),n,[&](int64_t i){ if(i>=lb) return g; if(i<=lb-r) return 1.0; return std::pow(g,double(i-(lb-r))/r);});
          }
      }
    bool open=false; const piece* prev=nullptr; uint64_t held=0;
    auto flush_tail=[&](){
      if(!open) return;
      uint64_t a=held-L/2;
      for(uint64_t p=a;p<held;++p)
        {
          double g=(held-p<=uint64_t(F))?double(held-p)/F:1.0;
          out.push_back(float(prev->at(p)*g));
        }
      open=false;
    };
    for(const piece& s: segs)
      {
        if(s.after_pause||!open)
          {
            flush_tail();
            size_t np=size_t(s.pause*pause_scale*sr/1000.0);
            out.insert(out.end(),np,0.0f);
            uint64_t a=s.a;
            uint64_t end=(s.b>a+L)?s.b-L/2:s.b;
            for(uint64_t p=a;p<end;++p)
              {
                double g=(p-a<uint64_t(F))?double(p-a)/F:1.0;
                out.push_back(float(s.at(p)*g));
              }
            held=s.b; prev=&s; open=(s.b>a+L);
            continue;
          }
        int h=L/2;
        uint64_t r0=held-h;
        bool can_ext=(held+h<=prev->A+prev->buf.size());
        int bestk=0; double bestc=-1e30;
        if(can_ext)
          for(int k=-M;k<=M;++k)
            {
              int64_t q0=int64_t(s.a)+k-h;
              if(q0<int64_t(s.A)||uint64_t(q0+L)>s.A+s.buf.size()) continue;
              double xy=0,yy=1e-9;
              for(int j=0;j<L;j+=2){ double x=prev->at(r0+j), y=s.at(q0+j); xy+=x*y; yy+=y*y; }
              double c=xy/std::sqrt(yy);
              if(c>bestc){bestc=c;bestk=k;}
            }
        int64_t q0=int64_t(s.a)+bestk-h;
        if(q0<int64_t(s.A)) q0=s.A;
        if(debug_joins) debug_joins->push_back(out.size()+h);
        for(int j=0;j<L;++j)
          {
            double wgt=0.5-0.5*std::cos(3.14159265358979323846*(j+0.5)/L);
            double x=prev->at(r0+j);
            if(!can_ext&&r0+j>=held) x=0;
            double y=s.at(q0+j);
            out.push_back(float(x*(1-wgt)+y*wgt));
          }
        uint64_t a=uint64_t(q0+L);
        uint64_t end=(s.b>a+L)?s.b-h:s.b;
        for(uint64_t p=a;p<end;++p) out.push_back(s.at(p));
        held=s.b; prev=&s; open=(s.b>a+L);
      }
    flush_tail();
    out.insert(out.end(),size_t(tail_pause*pause_scale*sr/1000.0),0.0f);
  }

  void synthesize(const voice& v,const std::vector<std::string>& labels,char utt_type,std::vector<float>& out,double pause_scale)
  {
    std::vector<target> T;
    for(size_t i=0;i<labels.size();++i)
      {
        target l,r;
        if(v.parse_label(labels[i],utt_type,l,r)){T.push_back(l);T.push_back(r);}
        else
          {
            l.phone=-1;
            l.pause_ms=(i==0)?30:(i+1==labels.size())?60:180;
            T.push_back(l);
          }
      }
    select_result s=select_units(v,T);
    concatenate(v,s,out,pause_scale);
  }
}
