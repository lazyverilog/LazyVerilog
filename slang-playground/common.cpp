#include <iostream>
#include <slang/util/BumpAllocator.h>
#include <slang/util/SmallVector.h>
#include <slang/util/Util.h>
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

    // std::unique_ptr<int> ptr = std::make_unique<int>(42);
    // not_null<int*> safe_ptr(ptr.get());

    not_null<int*> test_ptr = new int(42);
    std::cout << "Value: " << *test_ptr << "\n";
    delete test_ptr.get(); // don't forget to free the memory

    return 0;
}
