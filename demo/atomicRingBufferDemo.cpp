#include <AstraLib/AstraLib.hpp>
#include <thread>
#include <iostream>
struct IntStruct {
    int a;

    IntStruct() : a(0) {}
    IntStruct(int a_) : a(a_) {}
};
int main() {
    // The buffer size must be powers of two
    AstraLib::Buffers::AtomicRingBuffer<int, 1024> intBuffer;
    AstraLib::Buffers::AtomicRingBuffer<IntStruct,1024> structBuffer;

    intBuffer.enqueue(42);
    int value = intBuffer.dequeue();
    std::cout << "Dequeued value: " << value << std::endl;

    // Constructs the struct inplace resulting in no additional allocations
    structBuffer.emplaceEnqueue(1);
    IntStruct example = structBuffer.dequeue();
    std::cout << "Dequeued struct: " << example.a << std::endl;

    for(int i = 0; i <10 ; i++){
        intBuffer.enqueue(i);
    }

    // 50 is the default batchDequeue size for the array
    // it can be configured via adding a third parameter while constructing e.g. AstraLib::Buffers::AtomicRingBuffer<int,1024,128> example;
    // IMPORTANT: Since it exposes the raw pointer it doesn't provide any gurantees for multiple consumers requesting batchDequeue since it only uses a single array.
    // Therefore use this method only for MPSC applications.
    std::array<int,50>* arrayPtr = intBuffer.batchDequeue(10);

    std::cout << "Batch dequeued numbers: " << std::endl;
    for(int i = 0; i <10 ; i++){
        std::cout << arrayPtr->at(i)<< " ";
    }
    std::cout << std::endl;

    // clears the buffer via marking every slot empty
    // Doesn't call deconstructors 
    intBuffer.clearBuffer();

    if(intBuffer.isEmpty()) std::cout << "Returns true if buffer is empty" << std::endl;
}