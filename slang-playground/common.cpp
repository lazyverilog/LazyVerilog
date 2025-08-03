#include <iostream>
#include <slang/util/BumpAllocator.h>
#include <slang/util/SmallVector.h>
// #include <slang/util/Span.h>

using namespace std;
using namespace slang;

int main() {
    SmallVector<int> smallVec;
    smallVec.append(2, 3);
    smallVec.append(1, 4);
    smallVec.append(1, 5);

    for (int val : smallVec) {
        std::cout << val << " ";
    }

    BumpAllocator alloc;
    span<int> finalList = smallVec.copy(alloc);

    for (int val : finalList) {
        std::cout << val << " ";
    }

    return 0;
}
