#include <AstraLib/AstraLib.hpp>

int main() {
    AstraLib::Time::Timer timer;
    std::cout << std::endl;
    timer.start();
    std::cout << "Let's see how long this message took:" << std::endl;
    timer.write("It took");
    std::cout << std::endl;
    timer.start();
    std::cout << "This is the default view:" <<std::endl;
    timer.write();
}