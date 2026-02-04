import torch
import torchvision.models as models

model = models.resnet50(weights=None)
model.eval()

example = torch.randn(128, 3, 224, 224)
traced = torch.jit.trace(model, example)
traced.save("resnet50.pt")
