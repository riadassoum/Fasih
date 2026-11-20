"""Fine-tune Rawi v3 (Apache-2.0, TigreGotico) on extra vowelled text, CPU only.
usage: ft.py TRAIN.txt OUT.pth [hours] [lr]"""
import sys, json, time, random, unicodedata, math
import torch, torch.nn as nn
torch.set_num_threads(2)
R = "/home/claude/rawift"
voc = json.load(open(R + "/rawi_v3.vocab.json")); C2I, D2I = voc["char_to_idx"], voc["diac_to_idx"]
I2D = {v: k for k, v in D2I.items()}

class M(nn.Module):
    def __init__(s):
        super().__init__()
        s.embedding = nn.Embedding(len(C2I), 128, padding_idx=0)
        s.lstm = nn.LSTM(128, 256, 2, batch_first=True, bidirectional=True, dropout=0.3)
        s.dropout = nn.Dropout(0.3)
        s.presence_head = nn.Linear(512, 1); s.value_head = nn.Linear(512, len(D2I))
    def forward(s, x):
        h, _ = s.lstm(s.embedding(x)); h = s.dropout(h)
        return s.presence_head(h).squeeze(-1), s.value_head(h)

def pairs(line):
    t = "".join(c for c in unicodedata.normalize("NFD", line) if unicodedata.category(c) != "So")
    out = []
    for c in t:
        if unicodedata.category(c) == "Mn":
            if out: out[-1][1] += c
        else:
            out.append([c, ""])
    return out

def encode(line, L=300):
    p = pairs(line)[:L]
    return [C2I.get(c, C2I["<UNK>"]) for c, _ in p], [D2I.get(d, 0) for _, d in p]

def batches(lines, bs):
    random.shuffle(lines)
    lines.sort(key=len)  # bucket by length, then shuffle buckets
    bl = [lines[i:i + bs] for i in range(0, len(lines), bs)]
    random.shuffle(bl)
    for b in bl:
        enc = [encode(x) for x in b]
        T = max(len(e[0]) for e in enc)
        X = torch.zeros(len(enc), T, dtype=torch.long); Y = torch.full((len(enc), T), -100, dtype=torch.long)
        for i, (c, d) in enumerate(enc):
            X[i, :len(c)] = torch.tensor(c); Y[i, :len(d)] = torch.tensor(d)
        yield X, Y

if __name__ == "__main__":
    train, out = sys.argv[1], sys.argv[2]
    hours = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    lr = float(sys.argv[4]) if len(sys.argv) > 4 else 3e-4
    lines = [l.strip() for l in open(train, encoding="utf-8") if l.strip()]
    m = M(); m.load_state_dict(torch.load(R + "/diacritization_model_lstm_3.pth", map_location="cpu"))
    opt = torch.optim.Adam(m.parameters(), lr=lr, weight_decay=1e-5)
    bce, ce = nn.BCEWithLogitsLoss(), nn.CrossEntropyLoss()
    t0 = time.time(); step = 0; ep = 0; done = False
    while not done:
        ep += 1
        for X, Y in batches(lines, 64):
            m.train()
            real = Y != -100; marked = real & (Y != 0)
            vt = Y.clone(); vt[~marked] = -100
            p, v = m(X)
            loss = bce(p[real], marked[real].float()) + ce(v.reshape(-1, v.size(-1)), vt.reshape(-1))
            opt.zero_grad(); loss.backward(); nn.utils.clip_grad_norm_(m.parameters(), 5.0); opt.step()
            step += 1
            el = time.time() - t0
            for g in opt.param_groups:   # cosine decay over the time budget
                g["lr"] = lr * 0.5 * (1 + math.cos(math.pi * min(1.0, el / (hours * 3600))))
            if step % 200 == 0:
                print("ep %d step %d loss %.4f %.0f min" % (ep, step, loss.item(), el / 60), flush=True)
                torch.save(m.state_dict(), out)
            if el > hours * 3600:
                done = True; break
    torch.save(m.state_dict(), out); print("saved", out, "steps", step, "epochs", ep)
