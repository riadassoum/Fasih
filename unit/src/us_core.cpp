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
#include <map>
#include <array>
#include <cstdio>
#include <cstdlib>
#include "miniz/miniz.h"
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

  namespace
  {
    struct half_table
    {
      float t[65536];
      half_table()
      {
        for(uint32_t h=0;h<65536;++h)
          {
            uint32_t sg=(h&0x8000u)<<16, e=(h>>10)&0x1f, m=h&0x3ff, r;
            if(e==0){ if(m==0) r=sg; else { e=127-15+1; while(!(m&0x400)){m<<=1;--e;} m&=0x3ff; r=sg|(e<<23)|(m<<13);} }
            else if(e==31) r=sg|0x7f800000u|(m<<13);
            else r=sg|((e+112)<<23)|(m<<13);
            std::memcpy(&t[h],&r,4);
          }
      }
    };
    const half_table& halves(){ static const half_table t; return t; }
  }
  float half_to_float(uint16_t h){ return halves().t[h]; }

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
    bool mel=bool(std::ifstream(join_path(dir,"mel.bin"),std::ios::binary));
#ifdef FASIH_US_OPUS
    {
      std::ifstream t(join_path(dir,"audio.opa"),std::ios::binary);
      opus=bool(t);
    }
#endif
    if(mel) load_mel(join_path(dir,"mel.bin"));
    // a spectrum voice needs no audio; one with both keeps the audio for speaking without the vocoder
    if(opus||!mel||bool(std::ifstream(join_path(dir,"audio.pcm"),std::ios::binary)))
      { try { load_audio(join_path(dir,opus?"audio.opa":"audio.pcm")); } catch(...) { if(!mel) throw; } }
    for(auto& x: utts) n_samples=std::max<uint64_t>(n_samples,x.first+x.second);
    try{ load_weights(join_path(dir,"weights.txt")); } catch(...) {}
    try{ load_marks(join_path(dir,"marks.bin")); } catch(...) {mark_off.clear();}
    try{ load_models(join_path(dir,"models.bin")); } catch(...) {trees.clear(); questions.clear();}
    try{ load_phonesim(join_path(dir,"phonesim.txt")); } catch(...) {sim.clear();}
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
    for(int k=0;k<int(ctxdefs.size());++k)
      {
        auto it=ctxdefs[k].vals.find("pau");
        if(ctxdefs[k].name=="prev_name"){k_prev=k; pau_prev=(it==ctxdefs[k].vals.end())?-1:it->second;}
        if(ctxdefs[k].name=="next_name"){k_next=k; pau_next=(it==ctxdefs[k].vals.end())?-1:it->second;}
      }
    ctx_side.assign(nc,0);
    for(int k=0;k<nc;++k)
      {
        const std::string& n=ctxdefs[k].name;
        if(n=="prev_name"||n=="prev_prev_name"||n=="prev_syl_stress") ctx_side[k]=-1;
        if(n=="next_name"||n=="next_next_name"||n=="next_syl_stress") ctx_side[k]=1;
      }
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
        else if(p[0]=="join_pace") w.join_pace=v;
        else if(p[0]=="level_db") w.level_db=v;
        else if(p[0]=="join_register") w.join_register=v;
        else if(p[0]=="join_vuv") w.join_vuv=v;
        else if(p[0]=="join_base_mid") w.join_base_mid=v;
        else if(p[0]=="join_base_edge") w.join_base_edge=v;
        else if(p[0]=="join_base_stop") w.join_base_stop=v;
        else if(p[0]=="join_creak") w.join_creak=v;
        else if(p[0]=="target") w.target=v;
        else if(p[0]=="join") w.join=v;
        else if(p[0]=="max_cands") w.max_cands=int(v);
        else if(p[0]=="ling") w.ling=v;
        else if(p[0]=="vocoder") w.vocoder=v;
        else if(p[0]=="dur") w.dur=v;
        else if(p[0]=="f0") w.f0=v;
        else if(p[0]=="spec") w.spec=v;
        else if(p[0]=="quality") w.quality=v;
      }
  }

  void voice::load_units(const std::string& path)
  {
    auto f=open_in(path,std::ios::binary);
    char magic[8]; f.read(magic,8);
    int uver=0;
    if(std::memcmp(magic,"FSUV0002",8)==0) uver=2;
    else if(std::memcmp(magic,"FSUV0003",8)==0) uver=3;
    else throw std::runtime_error("fasih_us: bad units.bin");
    uint32_t hdr[5]; f.read((char*)hdr,sizeof(hdr));
    uint32_t nu=hdr[0], nutt=hdr[1]; jd=hdr[2]; uint32_t nc_file=hdr[3]; sr=hdr[4];
    if(jd>JD_MAX||int(nc_file)!=nc) throw std::runtime_error("fasih_us: units.bin does not match context.txt");
    std::vector<float> sd(jd); f.read((char*)sd.data(),4*jd);
    utts.resize(nutt); utt_pace.resize(nutt); utt_lf0.resize(nutt);
    for(uint32_t i=0;i<nutt;++i)
      {
        uint64_t b; uint32_t n; float pace,lf; f.read((char*)&b,8); f.read((char*)&n,4); f.read((char*)&pace,4); f.read((char*)&lf,4);
        utts[i]={b,n}; utt_pace[i]=std::log(std::max(pace,1.0f)); utt_lf0[i]=lf;
      }
    auto h2f=[](uint16_t h)->float{
      uint32_t s=(h&0x8000u)<<16, e=(h>>10)&0x1f, m=h&0x3ff, r;
      if(e==0){ if(m==0) r=s; else { e=127-15+1; while(!(m&0x400)){m<<=1;--e;} m&=0x3ff; r=s|(e<<23)|(m<<13);} }
      else if(e==31) r=s|0x7f800000u|(m<<13);
      else r=s|((e+112)<<23)|(m<<13);
      float x; std::memcpy(&x,&r,4); return x;
    };
    u.resize(nu);
    std::vector<uint16_t> hb(jd+1);
    if(uver==3)
      {
        // compact form: a bitmask of shared right edges, the fixed part of every unit, then unshared right edges
        std::vector<uint8_t> mask((nu+7)/8); f.read((char*)mask.data(),mask.size());
        const size_t fixed=16+nc+2*(jd+1);
        std::vector<char> rec(fixed);
        std::vector<std::array<uint16_t,JD_MAX+1>> left(nu);
        for(uint32_t i=0;i<nu;++i)
          {
            unit& x=u[i]; f.read(rec.data(),fixed);
            uint16_t p; uint8_t h,pad; uint32_t ut,a,b;
            std::memcpy(&p,&rec[0],2); h=rec[2]; pad=rec[3]; std::memcpy(&ut,&rec[4],4); std::memcpy(&a,&rec[8],4); std::memcpy(&b,&rec[12],4);
            x.phone=p; x.half=h; x.utt=ut; x.quality=pad; x.start=utts[ut].first+a; x.end=utts[ut].first+b;
            x.logdur=float(std::log(double(std::max<uint32_t>(b>a?b-a:1,1))));
            std::memset(x.ctx,0,sizeof(x.ctx)); std::memcpy(x.ctx,&rec[16],nc);
            std::memcpy(left[i].data(),&rec[16+nc],2*(jd+1));
            for(uint32_t k=0;k<jd;++k) x.jl[k]=left[i][k]; x.lf0l=h2f(left[i][jd]);
          }
        for(uint32_t i=0;i<nu;++i)
          {
            unit& x=u[i];
            bool shared=(mask[i>>3]>>(i&7))&1;
            if(shared) for(uint32_t k=0;k<=jd;++k) hb[k]=left[i+1][k];
            else f.read((char*)hb.data(),2*(jd+1));
            for(uint32_t k=0;k<jd;++k) x.jr[k]=hb[k]; x.lf0r=h2f(hb[jd]);
          }
      }
    else
    for(auto& x: u)
      {
        uint16_t p; uint8_t h,pad; uint32_t ut,a,b;
        f.read((char*)&p,2); f.read((char*)&h,1); f.read((char*)&pad,1); f.read((char*)&ut,4); f.read((char*)&a,4); f.read((char*)&b,4);
        x.phone=p; x.half=h; x.utt=ut; x.quality=pad;
        x.logdur=float(std::log(double(std::max<uint32_t>(b>a?b-a:1,1)))); x.start=utts[ut].first+a; x.end=utts[ut].first+b;
        std::memset(x.ctx,0,sizeof(x.ctx));
        f.read((char*)x.ctx,nc);
        f.read((char*)hb.data(),2*(jd+1)); for(uint32_t i=0;i<jd;++i) x.jl[i]=hb[i]; x.lf0l=h2f(hb[jd]);
        f.read((char*)hb.data(),2*(jd+1)); for(uint32_t i=0;i<jd;++i) x.jr[i]=hb[i]; x.lf0r=h2f(hb[jd]);
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
    if(map_size>=24&&std::memcmp(opa,"FSOP0002",8)==0) opa_version=2;
    else if(map_size<24||std::memcmp(opa,"FSOP0001",8)!=0) throw std::runtime_error("fasih_us: bad audio.opa");
    uint32_t h[4]; std::memcpy(h,opa+8,16);
    if(h[0]!=utts.size()||int(h[1])!=sr) throw std::runtime_error("fasih_us: audio.opa does not match units.bin");
    opa_frame=h[2]; opa_la=h[3];
    opa_utts.resize(h[0]);
    for(uint32_t i=0;i<h[0];++i){ std::memcpy(&opa_utts[i].off,opa+24+16*i,8); std::memcpy(&opa_utts[i].frames,opa+32+16*i,4); std::memcpy(&opa_utts[i].samples,opa+36+16*i,4);}
    for(auto& x: utts) n_samples=std::max<uint64_t>(n_samples,x.first+x.second);
    if(opa_version==2)
      {
        // packet offsets from the 16-bit sizes, once
        size_t total=0; for(auto& x: opa_utts) total+=x.frames+1;
        opa_offs.resize(total); size_t k=0;
        for(auto& x: opa_utts)
          {
            x.first_off=k; const uint16_t* sz=(const uint16_t*)(opa+x.off); uint32_t o=0;
            for(uint32_t i=0;i<x.frames;++i){ opa_offs[k++]=o; uint16_t z; std::memcpy(&z,sz+i,2); o+=z; }
            opa_offs[k++]=o;
          }
      }
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
    const uint32_t* offs; const unsigned char* data;
    if(opa_version==2){ offs=opa_offs.data()+U.first_off; data=opa+U.off+2*size_t(U.frames); }
    else { offs=(const uint32_t*)(opa+U.off); data=opa+U.off+4*(size_t(U.frames)+1); }
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

  void voice::load_mel(const std::string& path)
  {
#ifdef _WIN32
    HANDLE fh=CreateFileW(widen(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);
    if(fh==INVALID_HANDLE_VALUE) throw std::runtime_error("fasih_us: cannot open mel.bin");
    LARGE_INTEGER sz; GetFileSizeEx(fh,&sz);
    HANDLE mh=CreateFileMappingW(fh,nullptr,PAGE_READONLY,0,0,nullptr);
    mel_map=mh?(const unsigned char*)MapViewOfFile(mh,FILE_MAP_READ,0,0,0):nullptr;
    if(!mel_map){ if(mh) CloseHandle(mh); CloseHandle(fh); throw std::runtime_error("fasih_us: cannot map mel.bin"); }
    mel_fh=fh; mel_mh=mh; mel_size=size_t(sz.QuadPart);
#else
    int fd=::open(path.c_str(),O_RDONLY);
    if(fd<0) throw std::runtime_error("fasih_us: cannot open mel.bin");
    struct stat st; fstat(fd,&st);
    void* p=mmap(nullptr,st.st_size,PROT_READ,MAP_SHARED,fd,0); ::close(fd);
    if(p==MAP_FAILED) throw std::runtime_error("fasih_us: cannot map mel.bin");
    mel_map=(const unsigned char*)p; mel_size=st.st_size;
#endif
    if(mel_size<28||std::memcmp(mel_map,"FSMEL001",8)!=0) throw std::runtime_error("fasih_us: bad mel.bin");
    uint32_t n,K,hop; float q;
    std::memcpy(&n,mel_map+8,4); std::memcpy(&K,mel_map+12,4); std::memcpy(&q,mel_map+16,4); std::memcpy(&hop,mel_map+20,4);
    if(n!=utts.size()||hop!=256||K>100) throw std::runtime_error("fasih_us: mel.bin does not match units.bin");
    mel_K=K; mel_q=q;
    mel_sil.resize(100); std::memcpy(mel_sil.data(),mel_map+24,400);
    mel_recs.resize(n);
    for(uint32_t i=0;i<n;++i){ const unsigned char* r=mel_map+424+16*size_t(i); std::memcpy(&mel_recs[i].off,r,8); std::memcpy(&mel_recs[i].T,r+8,4); std::memcpy(&mel_recs[i].csize,r+12,4); }
    // orthonormal inverse DCT (DCT-III) of the first K coefficients onto 100 bands
    mel_idct.resize(size_t(K)*100);
    for(uint32_t k=0;k<K;++k)
      for(int b=0;b<100;++b)
        mel_idct[size_t(k)*100+b]=float((k==0?std::sqrt(1.0/100):std::sqrt(2.0/100))*std::cos(3.14159265358979323846*(b+0.5)*k/100));
  }

  std::shared_ptr<std::vector<float>> voice::decode_mel(uint32_t utt) const
  {
    {
      std::lock_guard<std::mutex> lock(mel_mutex);
      for(auto it=mel_cache.begin();it!=mel_cache.end();++it)
        if(it->first==utt){ mel_cache.splice(mel_cache.begin(),mel_cache,it); return mel_cache.front().second; }
    }
    const mel_rec& r=mel_recs[utt];
    const size_t ncoef=size_t(r.T)*mel_K;
    std::vector<unsigned char> raw(ncoef*5+16);
    mz_ulong rl=raw.size();
    if(mz_uncompress(raw.data(),&rl,mel_map+r.off,r.csize)!=MZ_OK) throw std::runtime_error("fasih_us: damaged mel.bin");
    auto out=std::make_shared<std::vector<float>>(size_t(r.T)*100,0.0f);
    std::vector<int32_t> prev(mel_K,0);
    size_t p=0;
    for(uint32_t t=0;t<r.T;++t)
      {
        float* row=out->data()+size_t(t)*100;
        for(uint32_t k=0;k<mel_K;++k)
          {
            uint32_t v=0; int sh=0;
            while(p<rl){ unsigned char b=raw[p++]; v|=uint32_t(b&127)<<sh; sh+=7; if(!(b&128)) break; }
            int32_t d=int32_t(v>>1)^-int32_t(v&1);
            prev[k]+=d;
            float c=prev[k]*mel_q;
            const float* basis=&mel_idct[size_t(k)*100];
            for(int b=0;b<100;++b) row[b]+=c*basis[b];
          }
      }
    std::lock_guard<std::mutex> lock(mel_mutex);
    mel_cache.emplace_front(utt,out);
    if(mel_cache.size()>64) mel_cache.pop_back();
    return out;
  }

  void voice::read_mel(uint32_t utt,int64_t f0,int64_t f1,float* out) const
  {
    auto m=decode_mel(utt); const int64_t T=mel_recs[utt].T;
    for(int64_t f=f0;f<f1;++f)
      {
        int64_t g=std::min<int64_t>(std::max<int64_t>(f,0),T-1);
        std::memcpy(out+size_t(f-f0)*100,m->data()+size_t(g)*100,400);
      }
  }

  void voice::load_marks(const std::string& path)
  {
    auto f=open_in(path,std::ios::binary);
    char magic[8]; f.read(magic,8);
    if(std::memcmp(magic,"FSPM0002",8)!=0) throw std::runtime_error("bad marks");
    uint32_t n; f.read((char*)&n,4);
    if(n!=utts.size()) throw std::runtime_error("marks do not match");
    mark_off.assign(1,0);
    std::vector<uint16_t> d;
    for(uint32_t i=0;i<n;++i)
      {
        uint32_t c; f.read((char*)&c,4);
        size_t o=mark_data.size(); mark_data.resize(o+c);
        if(c)
          {
            uint32_t first; f.read((char*)&first,4);
            d.resize(c-1); f.read((char*)d.data(),2*std::streamsize(d.size()));
            uint32_t v=first; mark_data[o]=v;
            for(uint32_t j=1;j<c;++j){ v+=d[j-1]; mark_data[o+j]=v; }
          }
        mark_off.push_back(mark_data.size());
      }
    if(!f) throw std::runtime_error("truncated marks");
  }

  void voice::load_models(const std::string& path)
  {
    auto f=open_in(path,std::ios::binary);
    char magic[8]; f.read(magic,8);
    if(std::memcmp(magic,"FSUM0001",8)!=0) throw std::runtime_error("bad models");
    uint32_t nq; f.read((char*)&nq,4);
    questions.resize(nq);
    for(auto& q: questions)
      {
        uint8_t t; uint16_t fi; f.read((char*)&t,1); f.read((char*)&fi,2);
        q.type=t; q.field=fi;
        if(t==0)
          {
            uint16_t n; f.read((char*)&n,2); std::string s(n,0); f.read(&s[0],n);
            for(auto& x: split_spaces(s)) q.set[x]=1;
          }
      }
    uint32_t nt; f.read((char*)&nt,4);
    trees.resize(nt);
    for(auto& t: trees)
      {
        uint32_t n; f.read((char*)&n,4); f.read((char*)&t.dim,4);
        t.sd.resize(t.dim); f.read((char*)t.sd.data(),4*t.dim);
        t.feat.resize(n); t.thr.resize(n); t.left.resize(n); t.right.resize(n); t.val.resize(size_t(n)*t.dim);
        f.read((char*)t.feat.data(),4*n); f.read((char*)t.thr.data(),4*n);
        f.read((char*)t.left.data(),4*n); f.read((char*)t.right.data(),4*n);
        f.read((char*)t.val.data(),4*size_t(n)*t.dim);
      }
    if(!f) throw std::runtime_error("truncated models");
  }

  void voice::load_phonesim(const std::string& path)
  {
    auto f=open_in(path); std::string a,b; float c;
    std::map<std::pair<std::string,std::string>,float> m;
    is_vowel.assign(phones.size(),0); is_stop.assign(phones.size(),0); is_closure.assign(phones.size(),0);
    std::string line;
    while(std::getline(f,line))
      {
        std::istringstream is(line);
        if(line.rfind("#vowels",0)==0){ is>>a; while(is>>a){ int id=phone_id(a); if(id>=0) is_vowel[id]=1; } continue; }
        if(line.rfind("#closures",0)==0){ is>>a; while(is>>a){ int id=phone_id(a); if(id>=0) is_closure[id]=1; } continue; }
        if(line.rfind("#stops",0)==0){ is>>a; while(is>>a){ int id=phone_id(a); if(id>=0) is_stop[id]=1; } continue; }
        if(is>>a>>b>>c) m[{a,b}]=c;
      }
    sim.assign(nc,{}); sim_n.assign(nc,0);
    for(int k=0;k<nc;++k)
      {
        const std::string& n=ctxdefs[k].name;
        if(n!="prev_name"&&n!="next_name"&&n!="prev_prev_name"&&n!="next_next_name") continue;
        int N=ctxdefs[k].vals.size(); sim_n[k]=N;
        std::vector<std::string> inv(N);
        for(auto& kv: ctxdefs[k].vals) inv[kv.second]=kv.first;
        sim[k].assign(size_t(N)*N,1.0f);
        for(int i=0;i<N;++i) for(int j=0;j<N;++j)
          {
            if(i==j){sim[k][size_t(i)*N+j]=0; continue;}
            auto it=m.find({inv[i],inv[j]});
            if(it!=m.end()) sim[k][size_t(i)*N+j]=it->second;
          }
      }
  }

  const float* voice::predict(int ti,const std::vector<std::string>& v,int half,char utt_type) const
  {
    const tree& t=trees[ti];
    int node=0;
    while(t.left[node]>=0)
      {
        const question& q=questions[t.feat[node]];
        float x=0;
        if(q.type==0) x=(q.field<int(v.size())&&q.set.count(v[q.field]))?1.0f:0.0f;
        else if(q.type==1)
          {
            const std::string& s=(q.field<int(v.size()))?v[q.field]:std::string("x");
            char* e=nullptr; long n=std::strtol(s.c_str(),&e,10);
            x=(e==s.c_str())?-1.0f:float(n);
          }
        else if(q.type==2) x=float(half);
        else x=(utt_type=='q')?1.0f:(utt_type=='e')?2.0f:0.0f;
        node=(x<=t.thr[node])?t.left[node]:t.right[node];
      }
    return t.val.data()+size_t(node)*t.dim;
  }

  void voice::build_index()
  {
    {
      std::vector<float> f;
      for(size_t i=0;i<u.size();i+=7) if(u[i].lf0l>0&&u[i].lf0r>0) f.push_back(0.5f*(u[i].lf0l+u[i].lf0r));
      if(!f.empty()){ std::nth_element(f.begin(),f.begin()+f.size()/10,f.end()); creak_lf0=f[f.size()/10]; }
    }
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
    // keep the wide lists short: an even sample (contiguous context copies are made below)
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
    for(auto* m: {&index,&index_wide})
      for(auto& kv: *m)
        {
          auto& c=pool_ctx[&kv.second]; c.resize(kv.second.size()*size_t(nc));
          for(size_t i=0;i<kv.second.size();++i) std::memcpy(&c[i*nc],u[kv.second[i]].ctx,nc);
        }
  }
  const uint8_t* voice::candidate_ctx(const std::vector<uint32_t>& pool) const
  {
    auto it=pool_ctx.find(&pool);
    return it==pool_ctx.end()?nullptr:it->second.data();
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
        if(has_models())
          {
            t.pred=true;
            t.pdur=predict(0,v,h,utt_type)[0];
            t.plf0=predict(1,v,h,utt_type)[0];
            const float* ps=predict(2,v,h,utt_type);
            for(int i=0;i<jd&&i<int(trees[2].dim);++i) t.pspec[i]=ps[i];
          }
      }
    return true;
  }

  namespace
  {
    // cost of one context feature of a unit (value val) against a target whose feature k is known
    double feature_cost(const voice& v,const target& t,int k,uint8_t val)
    {
      const weights& w=v.get_weights();
      if(w.ctx[k]==0||t.ctx[k]==val) return 0;
      double side=1.0;
      if(v.ctx_side[k])
        {
          bool vowel=!v.is_vowel.empty()&&t.phone>=0&&t.phone<int(v.is_vowel.size())&&v.is_vowel[t.phone];
          side=vowel?1.25:(((v.ctx_side[k]<0)==(t.half==0))?1.5:0.5);
        }
      double m=1.0;
      if(!v.sim.empty()&&v.sim_n[k]&&t.ctx[k]<v.sim_n[k]&&val<v.sim_n[k]) m=v.sim[k][size_t(t.ctx[k])*v.sim_n[k]+val];
      return w.ctx[k]*side*m;
    }

    double target_cost(const voice& v,const target& t,const unit& x,bool ling_only=false)
    {
      const weights& w=v.get_weights();
      double c=0;
      int nc=v.nctx();
      for(int k=0;k<nc;++k)
        {
          if(w.ctx[k]==0) continue;
          if(!t.known[k])
            {
              double side=1.0;
              if(v.ctx_side[k]){ bool vowel=!v.is_vowel.empty()&&t.phone>=0&&t.phone<int(v.is_vowel.size())&&v.is_vowel[t.phone];
                side=vowel?1.25:(((v.ctx_side[k]<0)==(t.half==0))?1.5:0.5);}
              c+=w.ctx[k]*side; continue;
            }
          c+=feature_cost(v,t,k,x.ctx[k]);
        }
      if(!t.pred||ling_only) return c;
      // hybrid: distance to the statistical predictions, in units of the models' own spread
      const auto& T=v.trees;
      double d=c*w.ling;
      d+=w.dur*std::fabs(x.logdur-t.pdur)/T[0].sd[0];
      if(x.lf0l>0&&x.lf0r>0) d+=w.f0*std::fabs(0.5*(x.lf0l+x.lf0r)-t.plf0)/T[1].sd[0];
      double e=0; int n=std::min<int>(v.jdim(),T[2].dim);
      for(int i=0;i<n;++i){ double z=(0.5*(half_to_float(x.jl[i])+half_to_float(x.jr[i]))-t.pspec[i])/T[2].sd[i]; e+=z*z; }
      d+=w.spec*std::sqrt(e/std::max(n,1));
      d+=w.quality*x.quality/20.0;
      return d;
    }

    double join_cost(const voice& v,uint32_t a,uint32_t b)
    {
      if(b==a+1) return 0;
      const auto& U=v.units();
      const unit& x=U[a]; const unit& y=U[b];
      const weights& w=v.get_weights();
      double d=0;
      { double e=(half_to_float(x.jr[0])-half_to_float(y.jl[0]))*w.join_energy; d+=e*e; }
      for(int i=1;i<v.jdim();++i){ double e=half_to_float(x.jr[i])-half_to_float(y.jl[i]); d+=e*e;}
      double c=w.join_spec*std::sqrt(d/v.jdim());
      bool va=x.lf0r>0, vb=y.lf0l>0;
      if(va&&vb) c+=w.join_f0*std::fabs(x.lf0r-y.lf0l)*10.0;
      else if(va!=vb) c+=w.join_vuv;
      c+=(y.half==1)?w.join_base_mid:w.join_base_edge;
      if(x.utt!=y.utt)
        {
          c+=w.join_pace*std::fabs(v.utt_pace[x.utt]-v.utt_pace[y.utt]);
          if(v.utt_lf0[x.utt]>0&&v.utt_lf0[y.utt]>0) c+=w.join_register*std::fabs(v.utt_lf0[x.utt]-v.utt_lf0[y.utt]);
        }
      return c;
    }
  }

  namespace
  {
    double target_cost_base(const voice& v,const target& t)
    {
      const weights& w=v.get_weights(); double c=0;
      for(int k=0;k<v.nctx();++k) if(w.ctx[k]!=0&&!t.known[k]){ double side=1.0;
          if(v.ctx_side[k]){ bool vowel=!v.is_vowel.empty()&&t.phone>=0&&t.phone<int(v.is_vowel.size())&&v.is_vowel[t.phone];
            side=vowel?1.25:(((v.ctx_side[k]<0)==(t.half==0))?1.5:0.5);}
          c+=w.ctx[k]*side; }
      return c;
    }
  }

  select_result select_units(const voice& v,const std::vector<target>& T,int64_t exclude_utt,int64_t prev_unit)
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
        // stage 1: context only, over the whole pool; stage 2: full acoustic cost for the best few hundred
        // the context cost depends only on the target: one table per half-phone, then lookups per candidate
        static thread_local std::vector<float> tab; tab.assign(size_t(v.nctx())*256,0.0f);
        for(int k=0;k<v.nctx();++k)
          if(t.known[k]) for(int val=0;val<256;++val) tab[size_t(k)*256+val]=float(feature_cost(v,t,k,uint8_t(val)));
        const int NCX=v.nctx();
        const uint8_t* pc=v.candidate_ctx(pool);
        for(size_t pi=0;pi<pool.size();++pi)
          {
            uint32_t id=pool[pi];
            if(exclude_utt>=0&&int64_t(U[id].utt)==exclude_utt) continue;
            const uint8_t* cx=pc?pc+pi*NCX:U[id].ctx; float c=0;
            for(int k=0;k<NCX;++k) c+=tab[size_t(k)*256+cx[k]];
            scored.push_back({double(c)+target_cost_base(v,t),id});
          }
        const size_t PRE=400;
        if(t.pred&&scored.size()>PRE)
          {
            std::nth_element(scored.begin(),scored.begin()+PRE,scored.end());
            scored.resize(PRE);
          }
        if(t.pred) for(auto& sc: scored) sc.first=target_cost(v,t,U[sc.second]);
        size_t K=std::min<size_t>(w.max_cands,scored.size());
        std::partial_sort(scored.begin(),scored.begin()+K,scored.end());
        scored.resize(K);
        if(i==0&&prev_unit>=0)
          {
            uint32_t q=uint32_t(prev_unit)+1;
            if(q<U.size()&&U[q].utt==U[prev_unit].utt&&U[q].phone==uint32_t(t.phone)&&U[q].half==t.half)
              scored.push_back({target_cost(v,t,U[q]),q});
          }
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
    // Viterbi. Join features of every candidate are unpacked once per step; previous candidates are
    // visited cheapest first, and since a join never costs less than nothing, the scan stops as soon as
    // a path is already dearer than the best one found: same result as the full search, much faster.
    const int jd=v.jdim();
    struct step_feats {std::vector<float> l,r; std::vector<float> lf0l,lf0r,pace,reg; std::vector<uint32_t> utt; std::vector<uint8_t> half,stop,creakl,creakr;};
    std::vector<step_feats> F(n);
    auto unpack=[&](size_t i){
      step_feats& S=F[i]; size_t c=cand[i].size();
      S.l.resize(c*jd); S.r.resize(c*jd); S.lf0l.resize(c); S.lf0r.resize(c); S.pace.resize(c); S.reg.resize(c); S.utt.resize(c); S.half.resize(c); S.stop.resize(c); S.creakl.resize(c); S.creakr.resize(c);
      for(size_t k=0;k<c;++k)
        {
          const unit& x=U[cand[i][k]];
          for(int d=0;d<jd;++d){ S.l[k*jd+d]=half_to_float(x.jl[d]); S.r[k*jd+d]=half_to_float(x.jr[d]); }
          S.l[k*jd]*=float(w.join_energy); S.r[k*jd]*=float(w.join_energy);
          S.lf0l[k]=x.lf0l; S.lf0r[k]=x.lf0r; S.utt[k]=x.utt; S.half[k]=x.half;
          S.stop[k]=(!v.is_stop.empty()&&x.phone<v.is_stop.size()&&v.is_stop[x.phone])?1:0;
          S.creakl[k]=(x.lf0l>0&&x.lf0l<v.creak_lf0)?1:0; S.creakr[k]=(x.lf0r>0&&x.lf0r<v.creak_lf0)?1:0;
          S.pace[k]=v.utt_pace[x.utt]; S.reg[k]=v.utt_lf0[x.utt];
        }
    };
    for(size_t i=0;i<n;++i) if(T[i].phone>=0) unpack(i);
    const float inv_jd=1.0f/jd;
    if(prev_unit>=0&&n>0&&T[0].phone>=0)
      {
        // the first step continues from the unit already spoken
        const unit& x=U[prev_unit];
        const step_feats& Q=F[0];
        for(size_t b=0;b<cand[0].size();++b)
          {
            double j=0;
            if(cand[0][b]!=uint32_t(prev_unit)+1)
              {
                float d=0;
                for(int k=0;k<jd;++k){ float xr=half_to_float(x.jr[k])*(k==0?float(w.join_energy):1.0f); float e=xr-Q.l[b*jd+k]; d+=e*e; }
                j=w.join_spec*std::sqrt(d*inv_jd);
                if(x.lf0r>0&&Q.lf0l[b]>0) j+=w.join_f0*std::fabs(x.lf0r-Q.lf0l[b])*10.0; else if((x.lf0r>0)!=(Q.lf0l[b]>0)) j+=w.join_vuv;
                j+=(Q.half[b]==1)?(Q.stop[b]?w.join_base_stop:w.join_base_mid):(Q.stop[b]?w.join_base_mid:w.join_base_edge);
                j*=w.join;
              }
            cost[0][b]+=j;
          }
      }
    int prev=-1;
    std::vector<int> order;
    for(size_t i=0;i<n;++i)
      {
        if(T[i].phone<0) continue;
        if(prev>=0&&size_t(prev)==i-1)
          {
            const step_feats& P=F[prev]; const step_feats& Q=F[i];
            order.resize(cand[prev].size());
            for(size_t a=0;a<order.size();++a) order[a]=a;
            std::sort(order.begin(),order.end(),[&](int x,int y){return cost[prev][x]<cost[prev][y];});
            std::vector<double> nc(cand[i].size(),INF);
            for(size_t b=0;b<cand[i].size();++b)
              {
                double best=INF; int arg=-1;
                const float* yl=&Q.l[b*jd];
                // inside a stop: expensive (two bursts or none); entering a stop's closure: as cheap as a mid-phone join
                const double base=(Q.half[b]==1)?(Q.stop[b]?w.join_base_stop:w.join_base_mid):(Q.stop[b]?w.join_base_mid:w.join_base_edge);
                for(int a: order)
                  {
                    double ca=cost[prev][a];
                    if(ca>=best) break;
                    double j;
                    if(cand[i][b]==cand[prev][a]+1) j=0;
                    else
                      {
                        const float* xr=&P.r[a*jd];
                        float d=0; for(int k=0;k<jd;++k){ float e=xr[k]-yl[k]; d+=e*e; }
                        j=w.join_spec*std::sqrt(d*inv_jd);
                        bool va=P.lf0r[a]>0, vb=Q.lf0l[b]>0;
                        if(va&&vb)
                          {
                            j+=w.join_f0*std::fabs(P.lf0r[a]-Q.lf0l[b])*10.0;
                            if(P.creakr[a]!=Q.creakl[b]) j+=w.join_creak;   // creaky against clear voice: heard as a stutter
                          }
                        else if(va!=vb) j+=w.join_vuv;
                        j+=base;
                        if(P.utt[a]!=Q.utt[b])
                          {
                            j+=w.join_pace*std::fabs(P.pace[a]-Q.pace[b]);
                            if(P.reg[a]>0&&Q.reg[b]>0) j+=w.join_register*std::fabs(P.reg[a]-Q.reg[b]);
                          }
                        j*=w.join;
                      }
                    double c=ca+j;
                    if(c<best){best=c;arg=a;}
                  }
                nc[b]=best+cost[i][b]; back[i][b]=arg;
              }
            cost[i].swap(nc);
          }
        else if(prev>=0)
          {
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

  namespace
  {
    double rms(const piece& s,int64_t a,int64_t b)
    {
      double e=0; int64_t n=0;
      for(int64_t p=a;p<b;++p){ double x=s.at(uint64_t(p)); e+=x*x; ++n;}
      return n?std::sqrt(e/n):0.0;
    }

    // multiply buf by a gain going from g (at edge) to 1 over r samples
    void gain_ramp(piece& s,int64_t edge,int64_t r,double g,bool head)
    {
      int64_t n=int64_t(s.buf.size());
      if(head)
        {
          for(int64_t i=0;i<n;++i)
            {
              int64_t d=i-edge; double w=(d<=0)?1.0:(d>=r?0.0:0.5+0.5*std::cos(3.14159265358979323846*d/r));
              s.buf[i]*=float(std::pow(g,w));
            }
        }
      else
        {
          for(int64_t i=0;i<n;++i)
            {
              int64_t d=edge-i; double w=(d<=0)?1.0:(d>=r?0.0:0.5+0.5*std::cos(3.14159265358979323846*d/r));
              s.buf[i]*=float(std::pow(g,w));
            }
        }
    }
  }

  void concatenate(const voice& v,const select_result& sel,std::vector<float>& out,double pause_scale,std::vector<size_t>* joins)
  {
    const auto& U=v.units();
    const int sr=v.sample_rate();
    const int H_MID=sr/100;      // half cross-fade inside a phone: 10 ms (20 ms fade)
    const int H_EDGE=sr/250;     // at a phone boundary: 4 ms (8 ms fade)
    const int H_MIN=sr/1000;     // never shorter than 1 ms on each side
    const int M=sr/200;          // +-5 ms search for the best waveform match
    const int R=sr*45/1000;      // pitch / loudness ramps on each side of a join
    const int EXT_OUT=sr*35/1000;// a phrase ends with the recording's own release (up to 35 ms)
    const int EXT_IN=sr*15/1000; // and starts with its own onset (up to 15 ms)
    const int PAD=H_MID+M+EXT_OUT+sr/100;
    std::vector<piece> segs;
    std::vector<uint32_t> first_u,last_u;
    double pending_pause=0; bool pause_flag=true;
    for(size_t i=0;i<sel.units.size();++i)
      {
        int64_t id=sel.units[i];
        if(id<0){pending_pause+=sel.pause_ms[i]; pause_flag=true; continue;}
        const unit& x=U[id];
        if(!segs.empty()&&!pause_flag&&i>0&&sel.units[i-1]==id-1)
          {segs.back().b=x.end; segs.back().f0b=x.lf0r>0?std::exp(x.lf0r):0; last_u.back()=id; continue;}
        piece p; p.a=x.start; p.b=x.end; p.utt=x.utt; p.after_pause=pause_flag; p.pause=pending_pause;
        p.f0a=x.lf0l>0?std::exp(x.lf0l):0; p.f0b=x.lf0r>0?std::exp(x.lf0r):0; p.A=0;
        segs.push_back(p); first_u.push_back(id); last_u.push_back(id);
        pending_pause=0; pause_flag=false;
      }
    double tail_pause=pending_pause;
    const size_t n=segs.size();
    // which joins are inside a phone (next piece starts with a right half)
    std::vector<char> mid(n,0), joined(n,0);
    for(size_t i=1;i<n;++i){ joined[i]=!segs[i].after_pause; mid[i]=joined[i]&&U[first_u[i]].half==1; }
    // pitch meeting points
    for(size_t i=1;i<n;++i)
      {
        if(!joined[i]) continue;
        piece& p=segs[i-1]; piece& q=segs[i];
        if(p.f0b<=0||q.f0a<=0) continue;
        double r=std::log(q.f0a/p.f0b);
        if(std::fabs(r)<0.015||std::fabs(r)>0.45) continue;
        double m=std::sqrt(p.f0b*q.f0a);
        p.gb=m/p.f0b; q.ga=m/q.f0a;
      }
    // buffers with margins, pitch bent near the joins
    for(piece& s: segs)
      {
        uint64_t us=v.utt_start(s.utt), ue=v.utt_end(s.utt);
        s.A=(s.a>us+PAD)?s.a-PAD:us;
        uint64_t B=std::min<uint64_t>(ue,s.b+PAD);
        s.buf.resize(B-s.A);
        v.read(s.utt,s.A,B,s.buf.data());
        if(!v.has_marks()) continue;
        int64_t la=int64_t(s.a-s.A), lb=int64_t(s.b-s.A), nb=int64_t(s.buf.size());
        int64_t r=std::min<int64_t>(R,(lb-la)/2);
        if(r<sr/200) continue;
        if(s.ga!=1.0)
          { double g=s.ga; psola(v,s,0,std::min(nb,la+r+sr/200),[&](int64_t i){ if(i<=la) return g; if(i>=la+r) return 1.0; return std::pow(g,double(la+r-i)/r);}); }
        if(s.gb!=1.0)
          { double g=s.gb; psola(v,s,std::max<int64_t>(0,lb-r-sr/200),nb,[&](int64_t i){ if(i>=lb) return g; if(i<=lb-r) return 1.0; return std::pow(g,double(i-(lb-r))/r);}); }
      }
    // loudness: inside a phone both sides meet at the same level
    for(size_t i=1;i<n;++i)
      {
        if(!mid[i]) continue;
        piece& p=segs[i-1]; piece& q=segs[i];
        int w=sr/50;
        double ep=rms(p,int64_t(p.b)-w,int64_t(p.b)), eq=rms(q,int64_t(q.a),int64_t(q.a)+w);
        if(ep<1e-4||eq<1e-4) continue;
        double r=eq/ep;
        if(r<0.5||r>2.0) continue;
        double gp=std::sqrt(r), gq=1.0/std::sqrt(r);   // p's end up, q's start down (or the reverse)
        int64_t rp=std::min<int64_t>(R,int64_t(p.b-p.a)), rq=std::min<int64_t>(R,int64_t(q.b-q.a));
        gain_ramp(p,int64_t(p.b-p.A),rp,gp,false);
        gain_ramp(q,int64_t(q.a-q.A),rq,gq,true);
      }
    // join geometry: half cross-fade h and lag k for every join
    std::vector<int> h(n,0), k(n,0);
    for(size_t i=1;i<n;++i)
      {
        if(!joined[i]) continue;
        const piece& p=segs[i-1]; const piece& q=segs[i];
        int64_t lp=int64_t(p.b-p.a), lq=int64_t(q.b-q.a);
        int hi=mid[i]?H_MID:H_EDGE;
        hi=int(std::min<int64_t>(hi,std::min(lp,lq)/2));
        if(hi<H_MIN) hi=H_MIN;
        h[i]=hi;
        int Mi=int(std::min<int64_t>(M,std::max<int64_t>(lq/4,1)));
        double best=-1e30; int bk=0;
        uint64_t r0=p.b-hi;
        for(int kk=-Mi;kk<=Mi;++kk)
          {
            int64_t q0=int64_t(q.a)+kk-hi;
            double xy=0,yy=1e-9,xx=1e-9;
            for(int j=0;j<2*hi;j+=2){ double x=p.at(r0+j), y=q.at(uint64_t(q0+j)); xy+=x*y; yy+=y*y; xx+=x*x;}
            double c=xy/std::sqrt(xx*yy);
            if(c>best){best=c;bk=kk;}
          }
        k[i]=bk;
      }
    // render
    const double PI=3.14159265358979323846;
    for(size_t i=0;i<n;++i)
      {
        const piece& s=segs[i];
        bool jn=(i+1<n)&&joined[i+1];
        int64_t begin=joined[i]?int64_t(s.a)+k[i]+h[i]:int64_t(s.a);
        int64_t end=jn?int64_t(s.b)-h[i+1]:int64_t(s.b);
        if(!joined[i])
          {
            // phrase start: pause, then the recording's own onset when it had a pause there
            int64_t ext=0;
            if(v.recorded_after_pause(first_u[i])) ext=std::min<int64_t>(EXT_IN,int64_t(s.a-s.A));
            double ms=s.pause*pause_scale;
            int64_t np=int64_t(ms*sr/1000.0)-ext; if(np<0){ext=std::max<int64_t>(0,ext+np); np=0;}
            out.insert(out.end(),size_t(np),0.0f);
            int64_t f=std::max<int64_t>(ext,sr/400);
            begin=int64_t(s.a)-ext;
            for(int64_t p=begin;p<end;++p)
              {
                double g=(p-begin<f)?0.5-0.5*std::cos(PI*double(p-begin)/f):1.0;
                out.push_back(float(s.at(uint64_t(p))*g));
              }
          }
        else
          for(int64_t p=begin;p<end;++p) out.push_back(s.at(uint64_t(p)));
        if(jn)
          {
            const piece& q=segs[i+1];
            int hh=h[i+1];
            int64_t r0=int64_t(s.b)-hh, q0=int64_t(q.a)+k[i+1]-hh;
            if(debug_joins) debug_joins->push_back(out.size()+hh);
            if(joins) joins->push_back(out.size()+hh);
            for(int j=0;j<2*hh;++j)
              {
                double w=0.5-0.5*std::cos(PI*(j+0.5)/(2*hh));
                out.push_back(float(s.at(uint64_t(r0+j))*(1-w)+q.at(uint64_t(q0+j))*w));
              }
          }
        else
          {
            // phrase end: the recording's own release when a pause followed it there
            int64_t ext=v.recorded_before_pause(last_u[i])?std::min<int64_t>(EXT_OUT,int64_t(s.A+s.buf.size()-s.b)):0;
            if(ext>0)
              {
                // only a real release: the recording must fade after the unit, not carry on (a drawn-out vowel)
                double before=rms(s,int64_t(s.b)-sr/100,int64_t(s.b)), after=rms(s,int64_t(s.b)+ext/2,int64_t(s.b)+ext);
                if(before<1e-4||after>0.35*before) ext=0;
              }
            int64_t f=ext?ext:std::min<int64_t>(sr/100,int64_t(s.b-s.a)/2);   // no release: a 10 ms fade, gentle on fricatives
            if(f<1) f=1;
            int64_t a=int64_t(s.b)-(ext?0:f);
            if(ext==0)
              { // no release available: fade the last 2.5 ms already written
                size_t m=std::min<size_t>(out.size(),size_t(f));
                for(size_t j=0;j<m;++j){ double g=0.5+0.5*std::cos(PI*double(j+1)/m); out[out.size()-m+j]*=float(g);} }
            else
              for(int64_t p=a;p<int64_t(s.b)+ext;++p)
                {
                  double g=0.5+0.5*std::cos(PI*double(p-a)/ext);
                  out.push_back(float(s.at(uint64_t(p))*g));
                }
            // the release counts towards the following pause
            if(i+1<n) segs[i+1].pause=std::max(0.0,segs[i+1].pause-1000.0*ext/sr/std::max(pause_scale,1e-3));
            else tail_pause=std::max(0.0,tail_pause-1000.0*ext/sr/std::max(pause_scale,1e-3));
          }
      }
    out.insert(out.end(),size_t(std::max(0.0,tail_pause*pause_scale)*sr/1000.0),0.0f);
    if(v.get_weights().level_db!=0.0)
      {
        float g=float(std::pow(10.0,v.get_weights().level_db/20.0));
        for(auto& x: out) x*=g;
      }
  }

  void concatenate_mel(const voice& v,const select_result& sel,std::vector<float>& frames,std::vector<size_t>& joins,double pause_scale)
  {
    const auto& U=v.units();
    const double hop=256.0;
    struct run{int64_t first,last; bool after_pause; double pause;};
    std::vector<run> runs;
    double pending=0; bool pflag=true;
    for(size_t i=0;i<sel.units.size();++i)
      {
        int64_t id=sel.units[i];
        if(id<0){ pending+=sel.pause_ms[i]; pflag=true; continue; }
        if(!runs.empty()&&!pflag&&i>0&&sel.units[i-1]==id-1){ runs.back().last=id; continue; }
        runs.push_back({id,id,pflag,pending}); pending=0; pflag=false;
      }
    const std::vector<float>& sil=v.mel_silence();
    auto silence=[&](double ms){ size_t n=size_t(std::lround(std::max(0.0,ms*pause_scale)*24000.0/1000.0/hop)); for(size_t k=0;k<n;++k) frames.insert(frames.end(),sil.begin(),sil.end()); };
    std::vector<float> buf;
    for(size_t r=0;r<runs.size();++r)
      {
        const run& R=runs[r];
        const unit& a=U[R.first]; const unit& b=U[R.last];
        const uint64_t base=v.utt_start(a.utt);
        int64_t fa=std::llround(double(a.start-base)/hop), fb=std::llround(double(b.end-base)/hop);
        bool ends_phrase=(r+1==runs.size())||runs[r+1].after_pause;
        if(R.after_pause&&v.recorded_after_pause(R.first)) fa-=1;       // the recording's own onset
        if(ends_phrase&&v.recorded_before_pause(R.last)) fb+=2;         // and its own release
        if(fb<=fa) fb=fa+1;
        if(R.after_pause) silence(R.pause);
        else joins.push_back(frames.size()/100);
        buf.resize(size_t(fb-fa)*100);
        v.read_mel(a.utt,fa,fb,buf.data());
        frames.insert(frames.end(),buf.begin(),buf.end());
      }
    silence(pending);
    if(frames.empty()) frames.insert(frames.end(),sil.begin(),sil.end());
    // the voice's level: a gain is a constant added to the log-magnitude spectrum
    if(v.get_weights().level_db!=0.0)
      {
        float g=float(v.get_weights().level_db/20.0*std::log(10.0));
        for(auto& x: frames) x+=g;
      }
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
