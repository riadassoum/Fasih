// fasih-us-cli VOICE_DIR LABELS OUT.wav [s|q|e] [rate]
// LABELS: RHVoice full-context labels, one per line (times optional; the label is the last field)
#include "us_core.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace fasih_us{extern std::vector<size_t>* debug_joins;}

static void write_wav(const std::string& path,const std::vector<float>& x,int sr)
{
  std::ofstream f(path,std::ios::binary);
  uint32_t n=x.size()*2;
  auto w32=[&](uint32_t v){f.write((char*)&v,4);};
  auto w16=[&](uint16_t v){f.write((char*)&v,2);};
  f.write("RIFF",4); w32(36+n); f.write("WAVEfmt ",8); w32(16); w16(1); w16(1); w32(sr); w32(sr*2); w16(2); w16(16);
  f.write("data",4); w32(n);
  for(float s: x){ long v=std::lround(s*32767.0); if(v>32767)v=32767; if(v<-32768)v=-32768; w16(uint16_t(int16_t(v))); }
}

int main(int argc,char** argv)
{
  if(argc<4){std::cerr<<"usage: fasih-us-cli VOICE LABELS OUT.wav [s|q|e]\n";return 1;}
  auto t0=std::chrono::steady_clock::now();
  fasih_us::voice v(argv[1]);
  auto t1=std::chrono::steady_clock::now();
  std::ifstream f(argv[2]); std::string l; std::vector<std::string> labs;
  while(std::getline(f,l)){ std::istringstream is(l); std::string tok,last; while(is>>tok) last=tok; if(!last.empty()) labs.push_back(last);}
  char ut=argc>4?argv[4][0]:'s';
  std::vector<float> out;
  {
    std::vector<fasih_us::target> T;
    for(size_t i=0;i<labs.size();++i){ fasih_us::target l,r; if(v.parse_label(labs[i],ut,l,r)){T.push_back(l);T.push_back(r);} else {l.phone=-1; l.pause_ms=(i==0)?30:(i+1==labs.size())?60:180; T.push_back(l);} }
    auto sel=fasih_us::select_units(v,T);
    if(getenv("DIAG")) for(size_t k=0;k<T.size();++k){ if(sel.units[k]<0) continue; const auto& u=v.units()[sel.units[k]];
      fprintf(stderr,"%zu ph%d h%d dur %.0fms pred %.0fms q %.2f utt %u\n",k,T[k].phone,T[k].half,(u.end-u.start)/24.0,T[k].pred?std::exp(T[k].pdur)/24.0:0.0,u.quality/20.0,u.utt);}
    int n=0,runs=0; int64_t prev=-2;
    for(auto id: sel.units){ if(id<0){prev=-2;continue;} ++n; if(id!=prev+1) ++runs; prev=id; }
    std::cerr<<"halves "<<n<<" runs "<<runs<<" avg run "<<(runs?double(n)/runs:0)<<" cost "<<sel.cost<<"\n";
    std::vector<size_t> J; fasih_us::debug_joins=&J;
    fasih_us::concatenate(v,sel,out);
    if(getenv("JOINS")){std::ofstream jf(std::string(argv[3])+".joins"); for(auto j: J) jf<<j<<"\n";}
  }
  auto t2=std::chrono::steady_clock::now();
  write_wav(argv[3],out,v.sample_rate());
  std::cerr<<"load "<<std::chrono::duration<double>(t1-t0).count()<<"s synth "<<std::chrono::duration<double>(t2-t1).count()*1000<<"ms audio "<<out.size()/double(v.sample_rate())<<"s\n";
}
