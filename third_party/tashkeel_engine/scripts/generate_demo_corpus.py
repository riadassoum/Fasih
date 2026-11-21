import argparse
import os
import random

# Diverse grammatically valid Arabic sentence templates for smoke testing & benchmarking
TEMPLATES = [
    "ذَهَبَ الطَّالِبُ إِلَى المَدْرَسَةِ فِي الصَّبَاحِ البَاكِرِ.",
    "كَتَبَ الكَاتِبُ الرِّسَالَةَ بِقَلَمٍ جَمِيلٍ عَلَى الوَرَقِ.",
    "يَقْرَأُ المُعَلِّمُ الكِتَابَ فِي المَكْتَبَةِ الكَبِيرَةِ.",
    "لَنْ يَنْجَحَ الكَسُولُ فِي الامْتِحَانِ النِّهَائِيِّ.",
    "لَمْ يَذْهَبْ أَحْمَدُ إِلَى السُّوقِ اليَوْمَ.",
    "إِنَّ العِلْمَ نُورٌ يَهْدِي الإِنْسَانَ إِلَى الخَيْرِ.",
    "كَانَ الجَوُّ جَمِيلًا فِي الرَّبِيعِ المَاضِي.",
    "فَتَحَ الوَلَدُ البَابَ وَدَخَلَ البَيْتَ بِسُرْعَةٍ.",
    "سَأَلَ التِّلْمِيذُ الأُسْتَاذَ عَنْ مَعْنَى الكَلِمَةِ.",
    "يَعْمَلُ المُهَنْدِسُ فِي المَشْرُوعِ الجَدِيدِ بِإِخْلَاصٍ.",
    "الشَّمْسُ مُشْرِقَةٌ وَالسَّمَاءُ صَافِيَةٌ فِي الصَّيْفِ.",
    "أَكَلَ الضَّيْفُ الطَّعَامَ الشَّهِيَّ فِي المَطْعَمِ.",
    "جَلَسَ القَاضِي فِي المَحْكَمَةِ لِيَحْكُمَ بِالعَدْلِ.",
    "رَجَعَ المُسَافِرُ مِنْ رِحْلَتِهِ سَالِمًا غَانِمًا.",
    "نَظَرَ البَاحِثُ فِي المَصَادِرِ القَدِيمَةِ وَالحَدِيثَةِ.",
    "يُحِبُّ النَّاسُ الصِّدْقَ وَالأَمَانَةَ فِي التَّعَامُلِ.",
    "بَنَى العُمَّالُ جِسْرًا طَوِيلًا فَوْقَ النَّهْرِ.",
    "حَضَرَتْ الطَّبِيبَةُ إِلَى المُسْتَشْفَى قَبْلَ المَوْعِدِ.",
    "لَعَلَّ النَّصْرَ قَرِيبٌ لِلصَّابِرِينَ.",
    "أَصْبَحَ البَرْنَامَجُ سَرِيعًا جِدًّا بَعْدَ التَّحْسِينِ.",
]

EXTRA_CLAUSES = [
    "وَقَدْ تَحَدَّثَ الجَمِيعُ عَنْ هَذَا الأَمْرِ بِاهْتِمَامٍ كَبِيرٍ.",
    "ثُمَّ عَادَ إِلَى مَنْزِلِهِ مَسْرُورًا بِالنَّتِيجَةِ.",
    "مِنْ أَجْلِ تَحْقِيقِ النَّجَاحِ فِي العَمَلِ.",
    "فِي الوَقْتِ المُنَاسِبِ دُونَ تَأْخِيرٍ.",
    "عَلَى الرَّغْمِ مِنَ الصُّعُوبَاتِ الكَثِيرَةِ.",
]


def main():
    parser = argparse.ArgumentParser(description="Generate demo diacritized train/test files")
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--train-lines", type=int, default=8000)
    parser.add_argument("--test-lines", type=int, default=500)
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    rng = random.Random(123)

    def gen_line():
        s = rng.choice(TEMPLATES)
        if rng.random() < 0.5:
            s = s[:-1] + " " + rng.choice(EXTRA_CLAUSES)
        return s

    train_path = os.path.join(args.out_dir, "demo_train.txt")
    test_path = os.path.join(args.out_dir, "demo_test.txt")

    with open(train_path, "w", encoding="utf-8") as f:
        for _ in range(args.train_lines):
            f.write(gen_line() + "\n")

    with open(test_path, "w", encoding="utf-8") as f:
        for _ in range(args.test_lines):
            f.write(gen_line() + "\n")

    print(f"Generated {train_path} ({args.train_lines} lines) and {test_path} ({args.test_lines} lines)")


if __name__ == "__main__":
    main()
