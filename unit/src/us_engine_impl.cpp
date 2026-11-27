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
#include "core/us_vocoder.hpp"
#include <cstdlib>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace RHVoice
{
  namespace
  {
    std::mutex cache_mutex;
    std::map<std::string,std::weak_ptr<const fasih_us::voice>> cache;

    bool exists(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0; }

    // the vocoder folder: FASIH_VOCODER_DIR (tests), else the fasihVocoder add-on installed with the first
    // premium voice (addons/fasihVocoder/vocoder), else a "vocoder" folder inside Fasih itself.
    // The library sits in addons/<fasih>/synthDrivers/fasih/lib/x64.
    std::string vocoder_dir(std::string& ort)
    {
      if(const char* d=std::getenv("FASIH_VOCODER_DIR"))
        { const char* o=std::getenv("FASIH_ORT_LIB"); ort=o?o:""; return d; }
      std::string here;
#ifdef _WIN32
      const char sep='\\'; const char* lib="fasih_ort.dll";
      HMODULE m=nullptr;
      if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&vocoder_dir,&m))
        {
          wchar_t buf[2048]; DWORD n=GetModuleFileNameW(m,buf,2048);
          int len=WideCharToMultiByte(CP_UTF8,0,buf,int(n),nullptr,0,nullptr,nullptr);
          here.assign(len,0); WideCharToMultiByte(CP_UTF8,0,buf,int(n),&here[0],len,nullptr,nullptr);
        }
#else
      const char sep='/'; const char* lib="libfasih_ort.so";
      Dl_info info;
      if(dladdr((void*)&vocoder_dir,&info)&&info.dli_fname) here=info.dli_fname;
#endif
      auto up=[&](std::string p,int k)->std::string{ for(int i=0;i<k;++i){ size_t s=p.find_last_of("\\/"); if(s==std::string::npos) return ""; p.resize(s);} return p; };
      std::string driver=up(here,3), addons=up(here,6);
      for(const std::string& d: {addons.empty()?std::string():addons+sep+"fasihVocoder"+sep+"vocoder", driver.empty()?std::string():driver+sep+"vocoder"})
        if(!d.empty()&&exists(d+sep+"vocos_core.onnx")){ ort=d+sep+lib; return d; }
      return "";
    }

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
    // neural smoothing: on for voices that ask for it (weights.txt "vocoder 1"), when the vocoder is installed
    if(db->get_weights().vocoder>0||db->has_mel())
      {
        std::string ort, dir=vocoder_dir(ort);
        if(!dir.empty()&&exists(dir+"/vocos_core.onnx")&&!ort.empty()&&exists(ort))
          { try { voc=fasih_us::vocoder::shared(ort,dir); } catch(...) { voc.reset(); } }
      }
  }

  void us_engine_impl::do_reset()
  {
  }

  bool us_engine_impl::vocoder_off() const
  {
    if(const char* e=std::getenv("FASIH_VOCODER")) return std::string(e)=="0";
    std::string ort, dir=vocoder_dir(ort);
    return dir.empty()||exists(dir+"/off");
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
    // the voice's own rate factor (weights.txt "rate"), so premium voices speak at the same pace as the others
    const double speed=((rate>0)?rate:1.0)*(db->get_weights().rate>0?db->get_weights().rate:1.0);
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
          std::size_t last=c0, minlen=voc?8:12;   // the vocoder costs time per piece: start with a shorter one
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
        std::vector<float> spoken_audio;   // context for the vocoder across streamed pieces
        std::vector<float> spoken_mel;     // the same for spectrum voices
        for(std::size_t ci=0;ok&&ci+1<cuts.size();++ci)
          {
            std::size_t a0=cuts[ci], a1=cuts[ci+1];
            std::size_t la=std::min<std::size_t>(c1,a1+(ci+2<cuts.size()?10:0));
            std::vector<fasih_us::target> sub(T.begin()+a0,T.begin()+la);
            fasih_us::select_result sel=fasih_us::select_units(*db,sub,-1,spoken);
            sel.units.resize(a1-a0); sel.pause_ms.resize(a1-a0);
            for(auto id: sel.units) if(id>=0) spoken=id;
            std::vector<float> out; std::vector<size_t> jn;
            if(db->has_mel())
              {
                // spectrum voice: the units' mel frames, made audible by the vocoder (always on for these voices)
                std::vector<float> frames;
                fasih_us::concatenate_mel(*db,sel,frames,jn);
                if(voc){ try { voc->process_mel(frames,jn,spoken_mel,out); } catch(...) { out.assign(frames.size()/100*256,0.0f); } }
                else out.assign(frames.size()/100*256,0.0f);   // no vocoder installed: silence rather than noise
                spoken_mel.insert(spoken_mel.end(),frames.begin(),frames.end());
                if(spoken_mel.size()>2400) spoken_mel.erase(spoken_mel.begin(),spoken_mel.end()-2400);
              }
            else
            fasih_us::concatenate(*db,sel,out,1.0,&jn);
            // neural smoothing (can be switched off without restarting: a file named "off" in the vocoder folder)
            if(!db->has_mel()&&voc&&!vocoder_off())
              {
                std::vector<float> y;
                try { voc->process(out,jn,spoken_audio,y,db->get_weights().vocoder>=2||(std::getenv("FASIH_VOCODER_JOINS")&&std::string(std::getenv("FASIH_VOCODER_JOINS"))=="1")); out.swap(y); } catch(...) {}
              }
            spoken_audio.insert(spoken_audio.end(),out.begin(),out.end());
            if(spoken_audio.size()>8192) spoken_audio.erase(spoken_audio.begin(),spoken_audio.end()-8192);
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
