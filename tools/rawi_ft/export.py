"""Export a fine-tuned Rawi v3 to ONNX with the ensemble's interface: input int64 [batch, seq]
-> gated_cls int64 [batch, seq] (0 = no mark). usage: export.py IN.pth OUT.onnx"""
import sys, torch
sys.path.insert(0, "/home/claude/rawift/work"); from ft import M
class G(torch.nn.Module):
    def __init__(s, m): super().__init__(); s.m = m
    def forward(s, x):
        p, v = s.m(x)
        return torch.where(p > 0, v.argmax(-1), torch.zeros_like(x))
m = M(); m.load_state_dict(torch.load(sys.argv[1], map_location="cpu")); m.eval()
torch.onnx.export(G(m), torch.randint(1, 200, (1, 50)), sys.argv[2], input_names=["input"], output_names=["gated_cls"],
                  dynamic_axes={"input": {0: "batch", 1: "seq"}, "gated_cls": {0: "batch", 1: "seq"}}, opset_version=17, dynamo=False)
print("exported", sys.argv[2])
