/* Fasih unit-selection backend for RHVoice.
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later, like RHVoice. */
#include <cmath>
#include <map>
#include <mutex>
#include <vector>
#include "core/us_engine_impl.hpp"
#include "core/us_core.hpp"
#include "core/voice.hpp"
#include "core/path.hpp"
#include "core/item.hpp"
#include "core/relation.hpp"
#include "core/utterance.hpp"
#include "sonic.h"

namespace RHVoice
{
  namespace
  {
    std::mutex cache_mutex;
    std::map<std::string,std::weak_ptr<const fasih_us::voice>> cache;

    std::shared_ptr<const fasih_us::voice> open_db(const std::string& dir)
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto& w=cache[dir];
      auto p=w.lock();
      if(!p){ p=std::make_shared<const fasih_us::voice>(dir); w=p; }
      return p;
    }
  }

  bool is_unit_voice(const voice_info& info)
  {
    return path::isfile(path::join(info.get_data_path(),"units.bin"));
  }

  us_engine_impl::us_engine_impl(const voice_info& info_):
    hts_engine_impl("unit",info_)
  {
  }

  hts_engine_impl::pointer us_engine_impl::do_create() const
  {
    return pointer(new us_engine_impl(info));
  }

  bool us_engine_impl::supports_quality(quality_t) const
  {
    return true;
  }

  sample_rate_t us_engine_impl::get_sample_rate_for_quality(quality_t) const
  {
    return sample_rate_24k;
  }

  void us_engine_impl::do_initialize()
  {
    db=open_db(data_path);
    sample_rate=static_cast<sample_rate_t>(db->sample_rate());
  }

  void us_engine_impl::do_reset()
  {
  }

  void us_engine_impl::do_synthesize()
  {
    const auto& utt=input->lbegin()->get_segment().get_relation().get_utterance();
    const std::string& ut=utt.get_utt_type();
    char type=(ut.find('q')!=std::string::npos&&ut!="whq")?'q':(ut=="e"?'e':'s');
    std::vector<fasih_us::target> T;
    std::vector<int> first_target;  // per label
    std::size_t nlab=0;
    for(auto it=input->lbegin();it!=input->lend();++it) ++nlab;
    std::size_t i=0;
    for(auto it=input->lbegin();it!=input->lend();++it,++i)
      {
        first_target.push_back(T.size());
        fasih_us::target l,r;
        if(db->parse_label(it->get_name(),type,l,r)){T.push_back(l);T.push_back(r);}
        else
          {
            l.phone=-1;
            double ms=(i==0)?30:(i+1==nlab)?60:180;
            const auto& seg=it->get_segment();
            if(seg.has_feature("break_time"))
              {
                int b=seg.get("break_time").as<int>();
                if(b>0) ms=b;
              }
            l.pause_ms=ms;
            T.push_back(l);
          }
      }
    first_target.push_back(T.size());
    // label of every target
    std::vector<int> lab_of(T.size());
    for(std::size_t l=0;l+1<first_target.size();++l)
      for(int k=first_target[l];k<first_target[l+1];++k) lab_of[k]=l;
    std::vector<label_sequence::iterator> labs;
    for(auto it=input->lbegin();it!=input->lend();++it) labs.push_back(it);
    const double speed=(rate>0)?rate:1.0;
    const double pitch=std::exp(pitch_shift);
    const bool mod=std::fabs(speed-1.0)>1e-3||std::fabs(pitch-1.0)>1e-3;
    sonicStream sn=nullptr;
    if(mod)
      {
        sn=sonicCreateStream(db->sample_rate(),1);
        sonicSetSpeed(sn,float(speed)); sonicSetPitch(sn,float(pitch)); sonicSetQuality(sn,1);
      }
    std::vector<float> rd(4096);
    double out_pos=0;   // samples before speed change
    auto emit=[&](const float* x,std::size_t n)->bool{
      const std::size_t block=1024;
      for(std::size_t a=0;a<n;a+=block)
        {
          if(output->is_stopped()) return false;
          std::size_t m=std::min(block,n-a);
          std::vector<double> d(x+a,x+a+m);
          if(eq) for(auto& v: d) v=eq->apply(v);
          try { output->process(d.data(),m); }
          catch(...) { output->stop(); return false; }
        }
      return true;
    };
    auto pass=[&](const float* x,std::size_t n,bool flush)->bool{
      if(!sn) return emit(x,n);
      if(n) sonicWriteFloatToStream(sn,const_cast<float*>(x),int(n));
      if(flush) sonicFlushStream(sn);
      int got;
      while((got=sonicReadFloatFromStream(sn,rd.data(),int(rd.size())))>0)
        if(!emit(rd.data(),got)) return false;
      return true;
    };
    // phrase by phrase: a pause cuts the search anyway, so each phrase is spoken as soon as it is ready
    std::size_t c0=0;
    bool ok=true;
    std::vector<int> timed(labs.size(),0);
    while(ok&&c0<T.size())
      {
        std::size_t c1=c0+1;
        while(c1<T.size()&&T[c1].phone>=0) ++c1;
        while(c1<T.size()&&T[c1].phone<0&&c1+1<T.size()&&T[c1+1].phone<0) ++c1; // keep runs of pauses together
        // a long phrase is cut where an unvoiced stop's (silent) closure begins, so it starts playing at once:
        // a short first piece, then longer ones; each is chosen with a look-ahead and continues from the unit
        // spoken last, so the choice is practically that of the whole phrase
        std::vector<std::size_t> cuts{c0};
        {
          std::size_t last=c0, minlen=12;
          for(std::size_t k=c0+1;k+8<c1;++k)
            {
              const auto& t=T[k];
              if(t.phone<0||t.half!=0||db->is_closure.empty()||!db->is_closure[t.phone]) continue;
              if(k-last<minlen) continue;
              cuts.push_back(k); last=k; minlen=40;
            }
        }
        cuts.push_back(c1);
        int64_t spoken=-1;
        for(std::size_t ci=0;ok&&ci+1<cuts.size();++ci)
          {
            std::size_t a0=cuts[ci], a1=cuts[ci+1];
            std::size_t la=std::min<std::size_t>(c1,a1+(ci+2<cuts.size()?10:0));
            std::vector<fasih_us::target> sub(T.begin()+a0,T.begin()+la);
            fasih_us::select_result sel=fasih_us::select_units(*db,sub,-1,spoken);
            sel.units.resize(a1-a0); sel.pause_ms.resize(a1-a0);
            for(auto id: sel.units) if(id>=0) spoken=id;
            std::vector<float> out;
            fasih_us::concatenate(*db,sel,out);
            double tot=0; std::vector<double> len(sel.units.size());
            for(std::size_t k=0;k<sel.units.size();++k)
              {
                if(sel.units[k]>=0){ const auto& u=db->units()[sel.units[k]]; len[k]=double(u.end-u.start);}
                else len[k]=sel.pause_ms[k]*db->sample_rate()/1000.0;
                tot+=len[k];
              }
            double scale=(tot>0)?out.size()/tot:1.0;
            double pos=out_pos;
            for(std::size_t k=0;k<sel.units.size();++k)
              {
                int l=lab_of[a0+k];
                if(!timed[l]){ labs[l]->set_time(int(pos/speed)); labs[l]->set_duration(0); timed[l]=1;}
                labs[l]->set_duration(labs[l]->get_duration()+int(len[k]*scale/speed));
                pos+=len[k]*scale;
              }
            out_pos+=out.size();
            ok=pass(out.data(),out.size(),a1>=T.size());
          }
        c0=c1;
      }
    if(sn) sonicDestroyStream(sn);
  }
}
