/* Fasih unit voices: optional neural smoothing (Vocos vocoder through ONNX Runtime).
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later.
   The model (Vocos, MIT licence, charactr) is run without its inverse STFT, which is done here, as is
   the mel analysis: 24 kHz, 1024-point FFT, hop 256, centred with reflect padding, 100 mel bands. */
#include "us_vocoder.hpp"
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "ort/onnxruntime_c_api.h"

namespace fasih_us
{
  namespace
  {
    const int N=1024, H=256, NB=513, NM=100;

    void fft(std::vector<std::complex<float>>& a,bool inv)
    {
      const size_t n=a.size();
      for(size_t i=1,j=0;i<n;++i){ size_t bit=n>>1; for(;j&bit;bit>>=1) j^=bit; j^=bit; if(i<j) std::swap(a[i],a[j]); }
      for(size_t len=2;len<=n;len<<=1)
        {
          double ang=2*3.14159265358979323846/len*(inv?1:-1);
          std::complex<float> wl(float(std::cos(ang)),float(std::sin(ang)));
          for(size_t i=0;i<n;i+=len)
            {
              std::complex<float> w(1,0);
              for(size_t j=0;j<len/2;++j){ auto u=a[i+j], v=a[i+j+len/2]*w; a[i+j]=u+v; a[i+j+len/2]=u-v; w*=wl; }
            }
        }
      if(inv) for(auto& x: a) x/=float(n);
    }

#ifdef _WIN32
    std::wstring widen(const std::string& s)
    {
      int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
      std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
      if(!w.empty()&&w.back()==0) w.pop_back();
      return w;
    }
#endif
  }

  struct vocoder::impl
  {
    void* lib{nullptr};
    const OrtApi* api{nullptr};
    OrtEnv* env{nullptr};
    OrtSession* sess{nullptr};
    OrtMemoryInfo* mem{nullptr};
    std::vector<float> win, fb;   // Hann window, mel filterbank (513 x 100)
    std::mutex run_mutex;

    void check(OrtStatus* s)
    {
      if(!s) return;
      std::string m=api->GetErrorMessage(s); api->ReleaseStatus(s);
      throw std::runtime_error("fasih_us vocoder: "+m);
    }

    // log-mel of x (centred frames, reflect padding): returns T frames of NM
    std::vector<float> mel(const std::vector<float>& x,size_t& T)
    {
      const int pad=N/2; const size_t L=x.size();
      auto at=[&](long i)->float{
        if(L<2) return 0;
        long n=long(L);
        while(i<0||i>=n){ if(i<0) i=-i; if(i>=n) i=2*(n-1)-i; }
        return x[size_t(i)];
      };
      T=1+L/H;
      std::vector<float> out(T*NM);
      std::vector<std::complex<float>> buf(N);
      std::vector<float> mag(NB);
      for(size_t t=0;t<T;++t)
        {
          long s=long(t*H)-pad;
          for(int k=0;k<N;++k) buf[k]=std::complex<float>(at(s+k)*win[k],0);
          fft(buf,false);
          for(int k=0;k<NB;++k) mag[k]=std::abs(buf[k]);
          for(int m=0;m<NM;++m)
            {
              float e=0; for(int k=0;k<NB;++k) e+=mag[k]*fb[size_t(k)*NM+m];
              out[t*NM+m]=std::log(std::max(e,1e-7f));
            }
        }
      return out;
    }

    // vocode T frames of log-mel (frame-major) into T*H samples (centred inverse STFT)
    std::vector<float> synth(const std::vector<float>& m,size_t T)
    {
      std::vector<float> in(NM*T);   // model wants (1, NM, T)
      for(size_t t=0;t<T;++t) for(int k=0;k<NM;++k) in[size_t(k)*T+t]=m[t*NM+k];
      int64_t shp[3]={1,NM,int64_t(T)};
      OrtValue* inp=nullptr; OrtValue* outs[2]={nullptr,nullptr};
      check(api->CreateTensorWithDataAsOrtValue(mem,in.data(),in.size()*sizeof(float),shp,3,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&inp));
      const char* in_names[]={"mel"}; const char* out_names[]={"re","im"};
      {
        std::lock_guard<std::mutex> lock(run_mutex);
        OrtStatus* st=api->Run(sess,nullptr,in_names,(const OrtValue* const*)&inp,1,out_names,2,outs);
        api->ReleaseValue(inp);
        check(st);
      }
      float* re=nullptr; float* im=nullptr;
      check(api->GetTensorMutableData(outs[0],(void**)&re)); check(api->GetTensorMutableData(outs[1],(void**)&im));
      const size_t Lfull=(T-1)*H+N;
      std::vector<float> y(Lfull,0.0f), env(Lfull,0.0f);
      std::vector<std::complex<float>> buf(N);
      for(size_t t=0;t<T;++t)
        {
          for(int k=0;k<NB;++k) buf[k]=std::complex<float>(re[size_t(k)*T+t],im[size_t(k)*T+t]);
          for(int k=1;k<N/2;++k) buf[N-k]=std::conj(buf[k]);
          buf[0]=std::complex<float>(buf[0].real(),0); buf[N/2]=std::complex<float>(buf[N/2].real(),0);
          fft(buf,true);
          for(int k=0;k<N;++k){ y[t*H+k]+=buf[k].real()*win[k]; env[t*H+k]+=win[k]*win[k]; }
        }
      api->ReleaseValue(outs[0]); api->ReleaseValue(outs[1]);
      std::vector<float> out(Lfull-N);
      for(size_t i=0;i<out.size();++i){ float e=env[i+N/2]; out[i]=(e>1e-11f)?y[i+N/2]/e:0.0f; }
      return out;
    }
  };

  vocoder::vocoder(const std::string& ort_lib,const std::string& dir,int threads): p(new impl)
  {
#ifdef _WIN32
    p->lib=(void*)LoadLibraryExW(widen(ort_lib).c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    auto getbase=p->lib?(const OrtApiBase*(*)())GetProcAddress((HMODULE)p->lib,"OrtGetApiBase"):nullptr;
#else
    p->lib=dlopen(ort_lib.c_str(),RTLD_NOW|RTLD_LOCAL);
    auto getbase=p->lib?(const OrtApiBase*(*)())dlsym(p->lib,"OrtGetApiBase"):nullptr;
#endif
    if(!getbase) throw std::runtime_error("fasih_us vocoder: cannot load ONNX Runtime from "+ort_lib);
    for(int v=ORT_API_VERSION;v>=17&&!p->api;--v) p->api=getbase()->GetApi(v);
    if(!p->api) throw std::runtime_error("fasih_us vocoder: ONNX Runtime too old");
    const OrtApi* api=p->api;
    p->check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"fasih_vocoder",&p->env));
    OrtSessionOptions* so=nullptr;
    p->check(api->CreateSessionOptions(&so));
    api->SetIntraOpNumThreads(so,threads);
    api->SetSessionGraphOptimizationLevel(so,ORT_ENABLE_ALL);
    std::string model=dir+"/vocos_core.onnx";
#ifdef _WIN32
    std::wstring wm=widen(model);
    OrtStatus* st=api->CreateSession(p->env,wm.c_str(),so,&p->sess);
#else
    OrtStatus* st=api->CreateSession(p->env,model.c_str(),so,&p->sess);
#endif
    api->ReleaseSessionOptions(so);
    p->check(st);
    p->check(api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&p->mem));
    p->win.resize(N);
    for(int k=0;k<N;++k) p->win[k]=float(0.5-0.5*std::cos(2*3.14159265358979323846*k/N));
    p->fb.resize(size_t(NB)*NM);
    // FILE* rather than ifstream: libc++ on MinGW has no wide-path ifstream constructor
#ifdef _WIN32
    FILE* f=_wfopen(widen(dir+"/melfb.f32").c_str(),L"rb");
#else
    FILE* f=std::fopen((dir+"/melfb.f32").c_str(),"rb");
#endif
    size_t got=f?std::fread(p->fb.data(),sizeof(float),p->fb.size(),f):0;
    if(f) std::fclose(f);
    if(got!=p->fb.size()) throw std::runtime_error("fasih_us vocoder: melfb.f32 missing");
  }

  vocoder::~vocoder()
  {
    if(!p||!p->api) return;
    if(p->mem) p->api->ReleaseMemoryInfo(p->mem);
    if(p->sess) p->api->ReleaseSession(p->sess);
    if(p->env) p->api->ReleaseEnv(p->env);
  }

  void vocoder::process(const std::vector<float>& x,const std::vector<size_t>& joins,const std::vector<float>& left,std::vector<float>& out)
  {
    // left context so that a streamed piece continues seamlessly from what was already spoken
    const size_t ctx=std::min<size_t>(left.size(),size_t(H)*24);
    std::vector<float> a(left.end()-ctx,left.end());
    a.insert(a.end(),x.begin(),x.end());
    size_t T=0;
    std::vector<float> m=p->mel(a,T);
    if(getenv("FASIH_DUMP_MEL")){ FILE* df=fopen(getenv("FASIH_DUMP_MEL"),"wb"); fwrite(m.data(),4,m.size(),df); fclose(df); }
    // blend the spectrum across every join: interpolate over three frames either side, half strength
    const std::vector<float> orig=m;
    for(size_t j: joins)
      {
        long f=long(std::lround(double(j+ctx)/H)), fa=f-3, fbk=f+3;
        if(fa<0||fbk>=long(T)) continue;
        for(long k=fa+1;k<fbk;++k)
          {
            float w=float(k-fa)/float(fbk-fa);
            for(int b=0;b<NM;++b)
              {
                float lin=(1-w)*orig[size_t(fa)*NM+b]+w*orig[size_t(fbk)*NM+b];
                m[size_t(k)*NM+b]=0.5f*lin+0.5f*orig[size_t(k)*NM+b];
              }
          }
      }
    std::vector<float> y=p->synth(m,T);
    out.assign(x.size(),0.0f);
    for(size_t i=0;i<x.size()&&i+ctx<y.size();++i) out[i]=y[i+ctx];
    // where the original is digital silence (pauses), keep it silent: the vocoder may leave faint noise
    const size_t W=H;
    for(size_t s=0;s+W<=x.size();s+=W)
      {
        float e=0; for(size_t i=s;i<s+W;++i) e=std::max(e,std::fabs(x[i]));
        if(e==0.0f) std::fill(out.begin()+s,out.begin()+s+W,0.0f);
      }
  }

  void vocoder::process_mel(const std::vector<float>& frames,const std::vector<size_t>& joins,const std::vector<float>& left,std::vector<float>& out)
  {
    const size_t ctxf=std::min<size_t>(left.size()/NM,24);
    std::vector<float> m(left.end()-ctxf*NM,left.end());
    m.insert(m.end(),frames.begin(),frames.end());
    const size_t T=m.size()/NM;
    const std::vector<float> orig=m;
    for(size_t j: joins)
      {
        long f=long(j+ctxf), fa=f-3, fbk=f+2;
        if(fa<0||fbk>=long(T)) continue;
        for(long k=fa+1;k<fbk;++k)
          {
            float w=float(k-fa)/float(fbk-fa);
            for(int b=0;b<NM;++b)
              {
                float lin=(1-w)*orig[size_t(fa)*NM+b]+w*orig[size_t(fbk)*NM+b];
                m[size_t(k)*NM+b]=0.5f*lin+0.5f*orig[size_t(k)*NM+b];
              }
          }
      }
    std::vector<float> y=p->synth(m,T);
    const size_t n=frames.size()/NM*H;
    out.assign(n,0.0f);
    for(size_t i=0;i<n&&i+ctxf*H<y.size();++i) out[i]=y[i+ctxf*H];
  }

  std::shared_ptr<vocoder> vocoder::shared(const std::string& ort_lib,const std::string& dir)
  {
    static std::mutex m; static std::map<std::string,std::weak_ptr<vocoder>> cache;
    std::lock_guard<std::mutex> lock(m);
    auto& w=cache[ort_lib+"|"+dir]; auto sp=w.lock();
    if(!sp){ sp=std::make_shared<vocoder>(ort_lib,dir,2); w=sp; }
    return sp;
  }
}
