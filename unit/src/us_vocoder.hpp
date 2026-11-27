/* Fasih unit voices: optional neural smoothing (Vocos vocoder through ONNX Runtime).
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later. */
#ifndef FASIH_US_VOCODER_HPP
#define FASIH_US_VOCODER_HPP
#include <string>
#include <vector>
#include <memory>

namespace fasih_us
{
  // Resynthesizes concatenated speech from its mel spectrum, blending the spectrum across the joins.
  // Loaded once, shared by all voices; safe to call from one thread at a time per instance.
  class vocoder
  {
  public:
    // ort_lib: path of the ONNX Runtime library; dir: folder with vocos_core.onnx and melfb.f32
    vocoder(const std::string& ort_lib,const std::string& dir,int threads=1);
    ~vocoder();
    // x: 24 kHz audio of one streamed piece; joins: sample positions of the joins in x.
    // left: audio already spoken just before x (context for a seamless seam), may be empty.
    void process(const std::vector<float>& x,const std::vector<size_t>& joins,const std::vector<float>& left,std::vector<float>& out);
    static std::shared_ptr<vocoder> shared(const std::string& ort_lib,const std::string& dir);
  private:
    struct impl; std::unique_ptr<impl> p;
  };
}
#endif
