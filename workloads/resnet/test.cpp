#include <torch/torch.h>
#include <iostream>

int main() {
    auto x = torch::randn({2, 3});
    std::cout << x << std::endl;
    return 0;
}
