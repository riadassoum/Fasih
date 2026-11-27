// fasih-us-usage VOICE LIST OUT : LIST lines "utt_index label_file type"; each sentence is
// synthesized without its own recording; OUT gets, per recording, the samples other sentences used.
#include "us_core.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
int main(int c,char** v){
  fasih_us::voice V(v[1]); std::ifstream L(v[2]); std::string line; std::vector<double> used(10000000,0);
  size_t nutt=0; int k=0;
  while(std::getline(L,line)){
    std::istringstream is(line); int64_t ui; std::string lf; char ty='s'; is>>ui>>lf>>ty;
    std::ifstream f(lf); std::string l; std::vector<std::string> labs;
    while(std::getline(f,l)){ std::istringstream s2(l); std::string t,last; while(s2>>t) last=t; if(!last.empty()) labs.push_back(last);}
    std::vector<fasih_us::target> T;
    for(size_t i=0;i<labs.size();++i){ fasih_us::target a,b; if(V.parse_label(labs[i],ty,a,b)){T.push_back(a);T.push_back(b);} else {a.phone=-1;a.pause_ms=0;T.push_back(a);} }
    auto sel=fasih_us::select_units(V,T,ui);
    for(auto id: sel.units) if(id>=0){ const auto& u=V.units()[id]; used[u.utt]+=double(u.end-u.start); nutt=std::max<size_t>(nutt,u.utt+1);}
    if(++k%500==0) std::cerr<<k<<"\n";
  }
  std::ofstream o(v[3]); for(size_t i=0;i<10000;++i) o<<used[i]<<"\n";
}
