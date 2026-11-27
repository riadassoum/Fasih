// fasih-us-encode VOICE_DIR [kbps] [voice|celt]
// celt: Opus's CELT mode, which keeps the 4-8 kHz band (س ش ص) at full strength; voice (SILK/hybrid)
// weakens it by about 1.7 dB at any bitrate. OPUS_MODE in train_voice.sh chooses.
// Compresses VOICE_DIR/audio.pcm into VOICE_DIR/audio.opa: every recording becomes an
// independent Opus stream of 20 ms frames, with a frame table for random access.
#include <opus.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc,char** argv)
{
  if(argc<2){std::cerr<<"usage: fasih-us-encode VOICE_DIR [kbps]\n";return 1;}
  std::string dir=argv[1];
  int kbps=argc>2?atoi(argv[2]):24;
  const bool celt=argc>3&&std::string(argv[3])=="celt";
  std::ifstream u(dir+"/units.bin",std::ios::binary);
  char magic[8]; u.read(magic,8);
  uint32_t hdr[5]; u.read((char*)hdr,20);
  uint32_t nutt=hdr[1], jd=hdr[2], sr=hdr[4];
  u.seekg(4*jd,std::ios::cur);
  std::vector<std::pair<uint64_t,uint32_t>> utts(nutt);
  for(auto& x: utts){u.read((char*)&x.first,8);u.read((char*)&x.second,4); float pf[2]; u.read((char*)pf,8);}
  // audio no unit can reach (pauses, leading and trailing silence) is encoded as digital silence
  uint32_t nu=hdr[0], nc=hdr[3];
  std::vector<std::vector<std::pair<uint32_t,uint32_t>>> keep(nutt);
  {
    const uint32_t M=sr/8;   // 125 ms around every unit: joins, ramps, releases and onsets stay intact
    // FSUV0002: every unit has both edges (4*(jd+1) bytes after its context); FSUV0003 (a finished,
    // compacted voice): a bitmap of shared right edges, then records with only the left edge
    const bool compact=std::string(magic,8)=="FSUV0003";
    if(compact) u.seekg((nu+7)/8,std::ios::cur);
    const std::streamoff tail=compact?2*(jd+1):4*(jd+1);
    for(uint32_t i=0;i<nu;++i)
      {
        char rec[16]; u.read(rec,16);
        uint32_t ut,a0,b0; std::memcpy(&ut,rec+4,4); std::memcpy(&a0,rec+8,4); std::memcpy(&b0,rec+12,4);
        u.seekg(nc+tail,std::ios::cur);
        if(ut<nutt) keep[ut].push_back({a0>M?a0-M:0,b0+M});
      }
  }
  std::ifstream a(dir+"/audio.pcm",std::ios::binary);
  uint64_t zeroed=0, totals=0;
  int err=0;
  OpusEncoder* enc=opus_encoder_create(sr,1,OPUS_APPLICATION_AUDIO,&err);
  if(err!=OPUS_OK){std::cerr<<"encoder: "<<opus_strerror(err)<<"\n";return 1;}
  opus_encoder_ctl(enc,OPUS_SET_BITRATE(kbps*1000));
  opus_encoder_ctl(enc,OPUS_SET_VBR(1));
  opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(10));
  if(celt) opus_encoder_ctl(enc,11002,1002);   // OPUS_SET_FORCE_MODE(MODE_CELT_ONLY)
  else opus_encoder_ctl(enc,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_int32 la=0; opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&la));
  const int FR=sr/50;
  std::FILE* o=std::fopen((dir+"/audio.opa").c_str(),"wb");
  std::fwrite("FSOP0001",1,8,o);
  uint32_t h2[4]={nutt,sr,uint32_t(FR),uint32_t(la)}; std::fwrite(h2,4,4,o);
  // table placeholder: per utterance u64 data offset, u32 frames, u32 samples
  long table=std::ftell(o);
  std::vector<uint64_t> uoff(nutt); std::vector<uint32_t> ufr(nutt);
  std::vector<char> zero(16*nutt,0); std::fwrite(zero.data(),1,zero.size(),o);
  std::vector<int16_t> x; std::vector<unsigned char> pkt(4000);
  uint64_t total=0;
  for(uint32_t i=0;i<nutt;++i)
    {
      opus_encoder_ctl(enc,OPUS_RESET_STATE);
      x.assign(utts[i].second,0);
      a.seekg(std::streamoff(utts[i].first*2));
      a.read((char*)x.data(),2*std::streamsize(x.size()));
      {
        std::vector<char> k(x.size(),0);
        for(auto& r: keep[i]) for(uint32_t p=r.first;p<std::min<uint32_t>(r.second,x.size());++p) k[p]=1;
        for(size_t p=0;p<x.size();++p) if(!k[p]){ x[p]=0; ++zeroed; }
        totals+=x.size();
      }
      size_t need=x.size()+la; size_t nf=(need+FR-1)/FR;
      x.resize(nf*FR,0);
      // per-utterance block: u32 frame offsets (relative to the block, nf+1 of them), then packets
      std::vector<uint32_t> offs(nf+1); std::vector<unsigned char> data;
      for(size_t f=0;f<nf;++f)
        {
          offs[f]=data.size();
          int n=opus_encode(enc,x.data()+f*FR,FR,pkt.data(),pkt.size());
          if(n<0){std::cerr<<"encode: "<<opus_strerror(n)<<"\n";return 1;}
          data.insert(data.end(),pkt.begin(),pkt.begin()+n);
        }
      offs[nf]=data.size();
      uoff[i]=std::ftell(o); ufr[i]=nf;
      std::fwrite(offs.data(),4,offs.size(),o);
      std::fwrite(data.data(),1,data.size(),o);
      total+=data.size();
      if(i%2000==0) std::cerr<<i<<"/"<<nutt<<"\n";
    }
  std::fseek(o,table,SEEK_SET);
  for(uint32_t i=0;i<nutt;++i){ std::fwrite(&uoff[i],8,1,o); std::fwrite(&ufr[i],4,1,o); std::fwrite(&utts[i].second,4,1,o);}
  std::fclose(o);
  opus_encoder_destroy(enc);
  std::cerr<<"silenced "<<100.0*zeroed/std::max<uint64_t>(totals,1)<<"% of the audio (never used)\n";
  std::cerr<<"done: "<<total/1048576.0<<" MB of packets, lookahead "<<la<<"\n";
}
