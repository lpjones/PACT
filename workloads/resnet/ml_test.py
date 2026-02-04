import torch
from transformers import GPTNeoForCausalLM
import time

torch.set_num_threads(16)

model = GPTNeoForCausalLM.from_pretrained(
    "EleutherAI/gpt-neo-2.7B",
    torch_dtype=torch.float32,
)

model.train()

optimizer = torch.optim.AdamW(model.parameters(), lr=1e-4)

# Dummy data
batch_size = 2
seq_len = 512

iterations = 4

for i in range(iterations):
    start_time = time.time()
    x = torch.randint(0, 50257, (batch_size, seq_len))
    y = x.clone()

    loss = model(x, labels=y).loss
    loss.backward()
    optimizer.step()
    optimizer.zero_grad()
    end_time = time.time()
    print(f"Iter time {i}: {end_time - start_time:.2f}s")
