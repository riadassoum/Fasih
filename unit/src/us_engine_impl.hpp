/* Fasih unit-selection backend for RHVoice.
   Copyright (C) 2026 Riad Assoum.  GPL-2.0-or-later, like RHVoice. */
#ifndef RHVOICE_US_ENGINE_IMPL_HPP
#define RHVOICE_US_ENGINE_IMPL_HPP
#include <memory>
#include <string>
#include "hts_engine_impl.hpp"

namespace fasih_us { class voice; }

namespace RHVoice
{
  class voice_info;

  // A voice folder containing units.bin is a unit-selection voice.
  bool is_unit_voice(const voice_info& info);

  class us_engine_impl: public hts_engine_impl
  {
  public:
    explicit us_engine_impl(const voice_info& info);
    bool supports_quality(quality_t q) const override;

  private:
    pointer do_create() const override;
    void do_initialize() override;
    void do_synthesize() override;
    void do_reset() override;
    sample_rate_t get_sample_rate_for_quality(quality_t q) const override;

    std::shared_ptr<const fasih_us::voice> db;
  };
}
#endif
