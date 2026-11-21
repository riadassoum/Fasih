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

## Second fine-tune (1.3): QCRI's modern Wikipedia

Continued from the first fine-tune (`INIT=ftA.pth ft.py train.txt ftQ.pth 2.5 1.5e-4`) on a new mix:

- 150,000 sentences of QCRI's diacritized Arabic Wikipedia (Mohamed and Mubarak, "Advancing Arabic Diacritization", EMNLP 2025, github.com/qcri/advancing-arabic-diacritization), vowelled by QCRI's best model.
- 150,000 lines of Rawi's own training text.
- 75,000 Shamela lines.
- The ArVoice text, repeated 8 times.

Hamzat al-wasl is written without a vowel throughout, and every test sentence was left out, including the WikiNews 2014 benchmark.

The checkpoint from about halfway through (75 minutes) was best on WikiNews. The shipped model averages its weights 70/30 with the first fine-tune, which kept most of the WikiNews gain while costing little on the rest.

Diacritic error rate / word error rate, case endings counted:

| Test set | First fine-tune (1.2/1.3 betas) | Halfway checkpoint | Shipped average (70/30) |
|---|---|---|---|
| WikiNews 2014, hand-vowelled modern news | 4.71% / 17.1% | 3.08% / 11.9% | 3.36% / 12.9% |
| Modern Arabic (ArVoice), 400 held-out sentences | 4.07% / 15.6% | 4.68% / 17.7% | 4.42% / 16.6% |
| ClArTTS book text | 6.49% / 20.5% | 6.49% / 20.1% | 6.34% / 19.7% |
| Rawi's own test set | 2.57% / 8.4% | 2.77% / 8.8% | 2.60% / 8.4% |
