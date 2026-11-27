// fasih-us-decode VOICE_DIR : rebuilds audio.pcm from audio.opa (FSOP0001 or FSOP0002), so a finished
// audio voice can be re-encoded (for instance at another bitrate) without its training folder.
#include <opus.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc,char** argv)
{
  if(argc<2){std::cerr<<"usage: fasih-us-decode VOICE_DIR\n";return 1;}
  std::string dir=argv[1];
  std::ifstream u(dir+"/units.bin",std::ios::binary);
  char magic[8]; u.read(magic,8); uint32_t hdr[5]; u.read((char*)hdr,20);
  uint32_t nutt=hdr[1], jd=hdr[2], sr=hdr[4];
  u.seekg(4*jd,std::ios::cur);
  std::vector<std::pair<uint64_t,uint32_t>> utts(nutt);
  for(auto& x: utts){u.read((char*)&x.first,8);u.read((char*)&x.second,4); float pf[2]; u.read((char*)pf,8);}
  std::ifstream f(dir+"/audio.opa",std::ios::binary);
  std::vector<unsigned char> opa((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
  bool v2=std::memcmp(opa.data(),"FSOP0002",8)==0;
  uint32_t h[4]; std::memcpy(h,opa.data()+8,16);
  const uint32_t FR=h[2], la=h[3];
  std::FILE* o=std::fopen((dir+"/audio.pcm").c_str(),"wb");
  int err=0; OpusDecoder* dec=opus_decoder_create(sr,1,&err);
  std::vector<opus_int16> fr(FR);
  for(uint32_t i=0;i<nutt;++i)
    {
      uint64_t off; uint32_t nf,ns;
      std::memcpy(&off,opa.data()+24+16*i,8); std::memcpy(&nf,opa.data()+32+16*i,4); std::memcpy(&ns,opa.data()+36+16*i,4);
      std::vector<uint32_t> offs(nf+1); const unsigned char* data;
      if(v2){ uint32_t s=0; for(uint32_t k=0;k<nf;++k){ offs[k]=s; uint16_t z; std::memcpy(&z,opa.data()+off+2*k,2); s+=z; } offs[nf]=s; data=opa.data()+off+2*size_t(nf); }
      else { std::memcpy(offs.data(),opa.data()+off,4*(nf+1)); data=opa.data()+off+4*(size_t(nf)+1); }
      opus_decoder_ctl(dec,OPUS_RESET_STATE);
      std::vector<opus_int16> y; y.reserve(size_t(nf)*FR);
      for(uint32_t k=0;k<nf;++k){ int n=opus_decode(dec,data+offs[k],int(offs[k+1]-offs[k]),fr.data(),FR,0); if(n>0) y.insert(y.end(),fr.begin(),fr.begin()+n); }
      std::vector<opus_int16> seg(utts[i].second,0);
      for(size_t p=0;p<seg.size()&&p+la<y.size();++p) seg[p]=y[p+la];
      std::fseek(o,long(utts[i].first*2),SEEK_SET);
      std::fwrite(seg.data(),2,seg.size(),o);
    }
  std::fclose(o); opus_decoder_destroy(dec);
  std::cerr<<"audio.pcm rebuilt: "<<nutt<<" recordings\n";
}
