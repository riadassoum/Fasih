/* Fasih unit-selection core (concatenative, non-neural).
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later, like RHVoice. */
#ifndef FASIH_US_CORE_HPP
#define FASIH_US_CORE_HPP
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace fasih_us
{
  constexpr int JD_MAX=16;
  constexpr int NC_MAX=32;

  struct unit
  {
    uint16_t phone;
    uint8_t half;
    uint32_t utt;
    uint64_t start, end;      // global sample positions
    uint8_t ctx[NC_MAX];
    float jl[JD_MAX]; float lf0l;   // join features at the start
    float jr[JD_MAX]; float lf0r;   // and at the end
  };

  struct target
  {
    int phone;       // -1 = pause
    int half;
    int neighbour;   // prev phone for a left half, next phone for a right half
    uint8_t ctx[NC_MAX];
    bool known[NC_MAX];
    double pause_ms; // for pauses
  };

  struct weights
  {
    double ctx[NC_MAX];
    double join_spec=1.0, join_energy=1.0, join_f0=6.0, join_vuv=2.0, join_base_mid=0.6, join_base_edge=1.2;
    double target=1.0, join=1.0;
    int max_cands=60;
  };

  class voice
  {
  public:
    explicit voice(const std::string& dir);
    int phone_id(const std::string& p) const;
    // label string -> target features; returns false for pauses
    bool parse_label(const std::string& lab, char utt_type, target& left, target& right) const;
    const std::vector<unit>& units() const {return u;}
    const std::vector<uint32_t>& candidates(int phone,int half,int neighbour) const;
    int16_t sample(uint64_t pos) const;
    // samples [a,b) (global positions inside recording utt) as floats in [-1,1)
    void read(uint32_t utt,uint64_t a,uint64_t b,float* out) const;
    const int16_t* samples(uint64_t pos) const;
    uint64_t total_samples() const {return n_samples;}
    int sample_rate() const {return sr;}
    int nctx() const {return nc;}
    int jdim() const {return jd;}
    const weights& get_weights() const {return w;}
    uint64_t utt_start(uint32_t i) const {return utts[i].first;}
    uint64_t utt_end(uint32_t i) const {return utts[i].first+utts[i].second;}
    // pitch marks of an utterance: positions relative to its start, *2, +1 when voiced
    const uint32_t* marks(uint32_t i,uint32_t& n) const
    {
      if(mark_off.empty()){n=0;return nullptr;}
      n=mark_off[i+1]-mark_off[i]; return mark_data.data()+mark_off[i];
    }
    bool has_marks() const {return !mark_off.empty();}
    ~voice();
  private:
    void load_units(const std::string& path);
    void load_audio(const std::string& path);
    void load_layout(const std::string& path);
    void load_context(const std::string& path);
    void load_weights(const std::string& path);
    void build_index();
    void load_marks(const std::string& path);
    std::vector<uint32_t> mark_data;
    std::vector<uint64_t> mark_off;

    int sr{24000}, jd{13}, nc{0};
    std::vector<unit> u;
    std::vector<std::pair<uint64_t,uint32_t>> utts;
    std::vector<std::string> phones;
    std::unordered_map<std::string,int> phone_ids;
    struct field {std::string name,left,right;};
    std::vector<field> layout;
    struct ctxdef {std::string name; int clip; std::unordered_map<std::string,int> vals; int field;};
    std::vector<ctxdef> ctxdefs;
    int f_name{-1}, f_prev{-1}, f_next{-1};
    std::unordered_map<uint64_t,std::vector<uint32_t>> index;      // phone,half,neighbour
    std::unordered_map<uint64_t,std::vector<uint32_t>> index_wide; // phone,half (sampled)
    std::vector<uint32_t> empty;
    weights w;
    // audio: memory-mapped or read into memory
    const int16_t* pcm{nullptr};
    uint64_t n_samples{0};
    std::vector<int16_t> pcm_mem;
    void* map_handle{nullptr};
    void* file_handle{nullptr};
    size_t map_size{0};
    bool opus{false};
    const unsigned char* opa{nullptr};
    uint32_t opa_frame{480}, opa_la{0};
    struct opa_utt{uint64_t off; uint32_t frames, samples;};
    std::vector<opa_utt> opa_utts;
  };

  struct select_result
  {
    std::vector<int64_t> units;   // -1 = pause
    std::vector<double> pause_ms;
    double cost{0};
  };

  select_result select_units(const voice& v,const std::vector<target>& t);

  // concatenate the selected units into 16-bit samples (pauses scaled by pause_scale)
  void concatenate(const voice& v,const select_result& sel,std::vector<float>& out,double pause_scale=1.0);

  // convenience: labels -> samples
  void synthesize(const voice& v,const std::vector<std::string>& labels,char utt_type,std::vector<float>& out,double pause_scale=1.0);
}
#endif
