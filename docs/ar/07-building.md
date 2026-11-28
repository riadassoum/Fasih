# بناء الإضافة والمحرك والنشر

## بناء ملف الإضافة

```
bash addon_src/build_release.sh 2.0 Fasih-2.0.nvda-addon
```

يبني هذا الأمر الإضافة من المستودع وحده، دون أي شيء مثبَّت سوى bash وpython3 وzip: الأصوات المرفقة في `voices/`، واللغتين في `lang/Arabic` و`lang_fr/French`، ومكتبات المحرك في `third_party/rhvoice-win-x64`، وراوي، وبرنامج التشغيل، وصفحات المساعدة (تُولَّد من `README.md` ومن `addon_src/doc/ar/readme.md`). اضبط `FASIH_SHARP=1` للإبقاء على رقع الشحذ حين تكون للأصوات.

رقم الإصدار هو الوسيط الأول، ويُكتب في `manifest.ini`.

## بناء المحرك

### للينكس (للتدريب والاختبار)

```
bash unit/build_engine.sh
```

يجلب RHVoice عند `engine/RHVOICE_BASE_COMMIT` مع أجزائه الفرعية اللازمة (Boost وSonic وأدوات CMake)، وOpus 1.5.2، إلى `build/`، ويطبّق تعديلات فصيح، ويبني:

- `build/rhvoice/b/src/lib/libRHVoice.so` ومكتبة النواة: المحرك الذي تحمّله أدوات التدريب والقياس.
- `build/rhvoice/b/src/utils/RHVoice-make-hts-labels`: أداة التسميات.
- `unit/bin/`: أدوات الأصوات الممتازة: `fasih-us-cli` (عميل اختبار) و`fasih-us-encode` و`fasih-us-decode` (ضغط Opus وفكّه) و`fasih-us-usage` (لتصغير الصوت).

يمكن استعمال نسخة RHVoice أخرى بتمرير مسارها: `bash unit/build_engine.sh /path/to/rhvoice linux`.

### لويندوز (لمكتبات الإضافة)

```
pip install ziglang==0.13.0
bash unit/build_engine.sh "" windows
cp build/rhvoice/bw/src/lib/libRHVoice.dll third_party/rhvoice-win-x64/RHVoice.dll
cp build/rhvoice/bw/src/core/libRHVoice_core.dll third_party/rhvoice-win-x64/
```

تُبنى مكتبات ويندوز بالترجمة المتقاطعة من لينكس بمترجم zig (clang لـx86_64-windows-gnu). لأن رابط zig يتجاهل تصدير كل الرموز، يكتب السكربت ملف `core.def` بكل رموز النواة: الدوال كدوال، والبيانات (جداول الدوال الافتراضية والمتغيّرات الثابتة) بعلامة DATA. هذا التمييز ضروري: حين صُدّرت البيانات كدوال، تعطّل كل نطق على ويندوز في نسخة 2.0 الأولى قبل نشرها، فلا تغيّره.

### اختبار مكتبات ويندوز دون ويندوز

يمكن تشغيل المكتبات تحت Wine 11 (من مستودع WineHQ، فـWine 9 في أوبونتو 24.04 لا يعمل في بعض البيئات): ابنِ برنامجًا صغيرًا بلغة C يستدعي `RHVoice_new_tts_engine` ثم `RHVoice_new_message` ثم `RHVoice_speak` بمترجم zig (`python3 -m ziglang cc -target x86_64-windows-gnu`)، وشغّله بـ`wine`. مرّر النص العربي في ملف لا في سطر الأوامر، لأن وسائط سطر الأوامر في ويندوز ليست UTF-8. هكذا تحقّقتُ من أن صوت ويندوز يطابق صوت لينكس عيّنةً عيّنة.

## اللغات

- العربية: `lang/build_lang.sh` يعيد توليد اللغة من مولّداتها. يحتاج foma، وأداتي RHVoice `att2bin` و`dtree2bin`، ونسخة RHVoice (الافتراضي `build/rhvoice` التي يجلبها `build_engine.sh`).
- الفرنسية: `lang_fr/build_lang.sh` بالطريقة نفسها، ويجلب معجم ipa-dict حين يغيب.

## الاختبارات

```
python3 tests/test_arabic.py
```

تختبر مسار النص العربي، والواجهة الفرنسية، والرقع، والمحدِّد، والتنزيل. اختبارات راوي الكاملة تحتاج `RAWI_ORT_LIB` و`FASIH_ADDON_DIR`.

## نشر إصدار

1. حدّث `CHANGELOG.md`: التغييرات التي يلاحظها المستخدم فقط، منذ آخر إصدار عام.
2. ابنِ الإضافة بـ`build_release.sh` بالرقم الجديد.
3. أنشئ إصدارًا على GitHub بوسم `vالرقم` (مثل `v2.0`) وارفع ملف `.nvda-addon`. «البحث عن تحديثات» في فصيح يقرأ أحدث إصدار من هناك، ويتحقّق من الحجم والمجموع الاختباري.
4. للأصوات: ارفع الحزم و`voices.json` إلى الإصدار «voices». لا يتغيّر رقم إصدار صوت إلا إذا تغيّرت ملفاته، فلا يُعرض تحديث لصوت لم يتغيّر.

## ملفات مساعدة أخرى في `tools/`

| الأداة | ما تفعله |
|---|---|
| `new_voice.sh`، `pipeline.sh` | مراحل تدريب الصوت المدمج الكاملة (يستدعيها `train_compact_voice.sh`) |
| `calib.sh` | معايرة تفاصيل الطيف على تسجيلات محجوزة |
| `ms_estimate.py` | قياس مرشّح التعديل لصوت |
| `eq_match.py` | مرشّح نبرة يقرّب صوتًا من أصوات مرجعية |
| `adjust_voice.py` | تغيير ارتفاع صوت مدمج وسرعته بعد التدريب |
| `eval_voice.py`، `latency.py` | توليد مقطع موحّد لكل صوت، وقياس زمن بدء النطق |
| `make_packs.py` | صنع حزم الأصوات و`voices.json` |
| `freeze_stats.py` | الإبقاء فقط على الإحصاءات اللازمة لإعادة كتابة صوت |
| `hts_edit.py`، `prebuilt.py` | مطابقة صوت جاهز (سرعة، ارتفاع، نبرة) دون بيانات تدريبه |
| `rawi_ft/` | ضبط راوي على نصوص فصيح (`RAWI_FT_DIR` يحدّد مجلد العمل) |
| `english_lexicon/` | الكلمات المضافة إلى المعجم الإنجليزي من قاموس CMU |
