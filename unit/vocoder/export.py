"""Export Vocos (charactr/vocos-mel-24khz, MIT) for Fasih's engine: the model without its inverse STFT,
which the engine does itself, plus the mel filterbank. Needs torch, vocos, onnx.
usage: python3 export.py OUTDIR"""
import sys, torch, numpy as np
from vocos import Vocos
out = sys.argv[1] if len(sys.argv) > 1 else "."
v = Vocos.from_pretrained("charactr/vocos-mel-24khz").eval()

class Core(torch.nn.Module):
    def __init__(s, v): super().__init__(); s.b = v.backbone; s.h = v.head
    def forward(s, mel):
        x = s.b(mel); x = s.h.out(x).transpose(1, 2); mag, p = x.chunk(2, dim=1)
        mag = torch.exp(mag).clip(max=1e2); return mag * torch.cos(p), mag * torch.sin(p)

torch.onnx.export(Core(v).eval(), (torch.randn(1, 100, 200),), out + "/vocos_core.onnx", input_names=["mel"], output_names=["re", "im"],
                  dynamic_axes={"mel": {2: "T"}, "re": {2: "T"}, "im": {2: "T"}}, opset_version=17, dynamo=False)
v.feature_extractor.mel_spec.mel_scale.fb.numpy().astype(np.float32).tofile(out + "/melfb.f32")   # (513, 100)
