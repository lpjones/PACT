#include <torch/torch.h>
#include <torch/script.h>

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

using clock_type = std::chrono::high_resolution_clock;

int main() {
    std::cout << "ResNet50 TorchScript benchmark" << std::endl;

    // ---------------- Configuration ----------------
    const int batch_size = 128;
    const int iters_per_epoch = 2;
    const int epochs = 2;

    torch::Device device(torch::kCPU);

    // Thread limits (important for allocator stability)
    torch::set_num_threads(1);
    torch::set_num_interop_threads(1);

    // ---------------- Load model ----------------
    torch::jit::Module model;
    try {
        model = torch::jit::load("/proj/TppPlus/tpp/libnuma_pgmig/workloads/resnet/build/resnet50.pt");
    } catch (const c10::Error& e) {
        std::cerr << "Error loading resnet50.pt\n";
        return -1;
    }

    model.to(device);
    model.train();

    // ---------------- Persistent input & target ----------------
    auto input = torch::randn(
        {batch_size, 3, 224, 224},
        torch::TensorOptions().device(device)
    );

    auto target = torch::randint(
        0, 1000,
        {batch_size},
        torch::TensorOptions().device(device).dtype(torch::kLong)
    );

    // ---------------- Loss ----------------
    auto criterion = torch::nn::CrossEntropyLoss();

    // ---------------- Optimizer (FIXED) ----------------
    std::vector<torch::Tensor> params;
    for (const auto& p : model.parameters()) {
        params.push_back(p);
    }

    torch::optim::SGD optimizer(
        params,
        torch::optim::SGDOptions(0.01)
    );

    // ---------------- Warm-up (allocate once) ----------------
    {
        std::vector<torch::jit::IValue> inputs;
        inputs.push_back(input);

        auto output = model.forward(inputs).toTensor();
        auto loss = criterion(output, target);

        // FIXED backward call
        loss.backward(/*gradient=*/{}, /*retain_graph=*/true);
    }

    // ---------------- Training loop (steady-state) ----------------
    for (int epoch = 1; epoch <= epochs; ++epoch) {
        auto start = clock_type::now();

        for (int i = 0; i < iters_per_epoch; ++i) {
            std::cout << "Iteration "
                      << (i + 1) << "/" << iters_per_epoch << std::endl;

            optimizer.zero_grad(/*set_to_none=*/false);

            std::vector<torch::jit::IValue> inputs;
            inputs.push_back(input);

            auto output = model.forward(inputs).toTensor();
            auto loss = criterion(output, target);

            // KEEP GRAPH ALIVE
            loss.backward(/*gradient=*/{}, /*retain_graph=*/true);
            optimizer.step();
        }

        auto end = clock_type::now();
        double seconds =
            std::chrono::duration<double>(end - start).count();

        double images_per_sec =
            (iters_per_epoch * batch_size) / seconds;

        std::cout << "Epoch " << epoch
                  << ": " << images_per_sec
                  << " images/sec" << std::endl;
    }

    std::cout << "Done. Sleeping..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));

    return 0;
}
