# Fine-tuning Rawi for Fasih

Fasih's diacritizer (`third_party/rawi/rawi_fasih.onnx`) is Rawi v3 by TigreGotico (Apache-2.0, https://huggingface.co/TigreGotico/rawi-v3) fine-tuned further on CPU for about 80 minutes with these scripts:

- `ft.py TRAIN.txt OUT.pth [hours] [lr]`: continues training from `diacritization_model_lstm_3.pth` (learning rate 3e-4, cosine decay over the time budget, batch 64).
- `export.py IN.pth OUT.onnx`: exports with the same interface as the Rawi ensemble (input ids -> `gated_cls`), so `rawi_host` loads it unchanged.
- `der_onnx.py TEST.txt MODEL.onnx...`: diacritic and word error rates on fully vowelled text.

Training mix (one line per sentence, at most 280 characters): about 9,200 modern sentences from the ArVoice texts (Toyin et al., MBZUAI, CC BY 4.0) repeated 8 times, 75,000 fully vowelled lines from Shamela books (ReligiousLLMs/shamela_all_diacritized_fully, first two parts), and 150,000 lines of Rawi's own training text (TigreGotico/arabic_diacritized_text) so the model keeps what it knew. The sentences of the modern test set were left out of training.

Results (diacritic error rate / word error rate):

| Test set | Rawi ensemble (1.2) | Rawi v3 | Fasih fine-tune |
|---|---|---|---|
| Modern Arabic, 400 held-out sentences | 3.99% / 15.5% | 3.91% / 15.1% | 3.12% / 12.0% |
| ClArTTS book text (not in training) | 6.47% / 20.3% | 6.35% / 20.0% | 3.51% / 12.6% |
| Rawi's own test set | 2.42% / 8.1% | 2.45% / 8.2% | 2.63% / 8.9% |

It is also about twice as fast as the ensemble.
