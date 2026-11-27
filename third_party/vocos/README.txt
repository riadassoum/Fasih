Vocos (https://github.com/gemelo-ai/vocos), model charactr/vocos-mel-24khz, MIT licence (LICENSE).
vocos_core.onnx is the model without its final inverse STFT (done in Fasih's engine, unit/src/us_vocoder.cpp); melfb.f32 is its 100-band mel filterbank. Exported with unit/vocoder/export.py.
