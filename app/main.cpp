// 联调程序入口（A/B 共同维护）：目前仅显示骨架状态，不接受或执行 SQL。
#include <iostream>

int main(int argc, char*[]) {
    if (argc != 1) {
        std::cerr << "SQL input is not supported by this scaffold yet.\n";
        return 2;
    }
    std::cout << "MiniSQL compiler scaffold\n"
              << "A: Lexer / Parser -> AST (NotImplemented)\n"
              << "B: Semantic / Plan generation (NotImplemented)\n"
              << "Run contracts_example for manual AST / Bound / Plan examples.\n";
    return 0;
}
