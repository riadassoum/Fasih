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
  constexpr int JD_MAX=14;
  constexpr int NC_MAX=24;
  float half_to_float(uint16_t h);

  struct unit
  {
    uint16_t phone;
    uint8_t half;
    uint8_t quality;                // quality cost * 20 (0 = typical unit)
    uint32_t utt;
    uint64_t start, end;      // global sample positions
    uint8_t ctx[NC_MAX];
    uint16_t jl[JD_MAX];            // join features at the start (half precision)
    uint16_t jr[JD_MAX];            // and at the end
    float lf0l, lf0r;               // log F0 at the edges (<=0: unvoiced)
    float logdur;                   // log length in samples
  };

  struct target
  {
    int phone;       // -1 = pause
    int half;
    int neighbour;   // prev phone for a left half, next phone for a right half
    uint8_t ctx[NC_MAX];
    bool known[NC_MAX];
    double pause_ms; // for pauses
    bool pred{false};            // statistical targets available
    float pdur, plf0, pspec[JD_MAX];
  };

  struct weights
  {
    double ctx[NC_MAX];
    double join_base_stop=4.0, join_creak=2.5, join_spec=1.0, join_energy=1.0, join_pace=3.0, join_register=4.0, level_db=0.0, join_f0=6.0, join_vuv=2.0, join_base_mid=0.6, join_base_edge=1.2;
    double target=1.0, join=1.0;
    int max_cands=60;
    double vocoder=0;   // 1: neural smoothing when installed
    double ling=0.5, dur=1.0, f0=1.0, spec=1.0, quality=1.0;   // hybrid target cost
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
    // the context bytes of a candidate list, contiguous (nctx per candidate), for fast scanning
    const uint8_t* candidate_ctx(const std::vector<uint32_t>& pool) const;
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
    // in its recording, is this unit's phone preceded / followed by a pause?
    bool recorded_after_pause(uint32_t id) const {return k_prev>=0&&pau_prev>=0&&u[id].ctx[k_prev]==pau_prev;}
    bool recorded_before_pause(uint32_t id) const {return k_next>=0&&pau_next>=0&&u[id].ctx[k_next]==pau_next;}
    ~voice();
  private:
    void load_units(const std::string& path);
    void load_audio(const std::string& path);
    void load_layout(const std::string& path);
    void load_context(const std::string& path);
    void load_weights(const std::string& path);
    void build_index();
    void load_marks(const std::string& path);
    void load_models(const std::string& path);
    void load_phonesim(const std::string& path);
  public:
    struct question {int type; int field; std::unordered_map<std::string,int> set;};
    struct tree {uint32_t dim; std::vector<float> sd; std::vector<int32_t> feat,left,right; std::vector<float> thr,val;};
    std::vector<question> questions;
    std::vector<tree> trees;      // duration, pitch, spectrum
    bool has_models() const {return trees.size()==3;}
    // phonetic substitution cost for phone-name context features: sim[k] is n*n over that feature's vocabulary
    std::vector<std::vector<float>> sim; std::vector<int> sim_n;
    std::vector<int> ctx_side;
    std::vector<char> is_vowel;  // by phone id
    std::vector<char> is_stop;   // by phone id
    std::vector<char> is_closure;
    float creak_lf0{0};          // log F0 below which a unit counts as creaky (speaker's 10th percentile)// unvoiced stops: silent closure, safe place to cut for streaming   // -1 previous side, +1 next side, 0 other
    const float* predict(int t,const std::vector<std::string>& v,int half,char utt_type) const;
  private:
    std::vector<uint32_t> mark_data;
    std::vector<uint64_t> mark_off;

    int sr{24000}, jd{13}, nc{0};
    std::vector<unit> u;
    std::vector<std::pair<uint64_t,uint32_t>> utts;
  public:
    std::vector<float> utt_pace, utt_lf0;   // log phones per second, log median F0
  private:
    std::vector<std::string> phones;
    std::unordered_map<std::string,int> phone_ids;
    struct field {std::string name,left,right;};
    std::vector<field> layout;
    struct ctxdef {std::string name; int clip; std::unordered_map<std::string,int> vals; int field;};
    std::vector<ctxdef> ctxdefs;
    int f_name{-1}, f_prev{-1}, f_next{-1};
    int k_prev{-1}, k_next{-1}, pau_prev{-1}, pau_next{-1};
    std::unordered_map<uint64_t,std::vector<uint32_t>> index;      // phone,half,neighbour
    std::unordered_map<uint64_t,std::vector<uint32_t>> index_wide; // phone,half (sampled)
    std::vector<uint32_t> empty;
    std::unordered_map<const std::vector<uint32_t>*,std::vector<uint8_t>> pool_ctx;
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
    struct opa_utt{uint64_t off; uint32_t frames, samples; uint64_t first_off;};
    int opa_version{1};
    std::vector<uint32_t> opa_offs;   // compact audio: packet offsets rebuilt at load
    std::vector<opa_utt> opa_utts;
  };

  struct select_result
  {
    std::vector<int64_t> units;   // -1 = pause
    std::vector<double> pause_ms;
    double cost{0};
  };

  // prev_unit: the unit already spoken just before t[0] (chunked streaming), or -1
  select_result select_units(const voice& v,const std::vector<target>& t,int64_t exclude_utt=-1,int64_t prev_unit=-1);

  // concatenate the selected units into 16-bit samples (pauses scaled by pause_scale)
  void concatenate(const voice& v,const select_result& sel,std::vector<float>& out,double pause_scale=1.0,std::vector<size_t>* joins=nullptr);

  // convenience: labels -> samples
  void synthesize(const voice& v,const std::vector<std::string>& labels,char utt_type,std::vector<float>& out,double pause_scale=1.0);
}
#endif
