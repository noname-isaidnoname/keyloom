#include "uinput.hpp"
#include "errors.hpp"

#include <cstdlib>
#include <iostream>
#include <linux/input.h>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: uinput_fault_probe <expected-step> [default|rel|abs|phys]\n";
        return 2;
    }
    const std::string expected = argv[1];
    const std::string config = argc >= 3 ? argv[2] : "default";
    if (::access("/dev/uinput", W_OK) != 0) {
        std::cout << "skip: /dev/uinput not writable\n";
        return 77;
    }
    std::string phys = config == "phys" ? "keyloom/test/fault/phys" : "keyloom/test/fault";
    std::vector<int> relative = config == "rel" ? std::vector<int>{REL_X} : std::vector<int>{};
    std::vector<keyloom::AbsAxis> absolute =
        config == "abs" ? std::vector<keyloom::AbsAxis>{{ABS_X, 0, 1919, 0, 0, 0}} : std::vector<keyloom::AbsAxis>{};
    try {
        keyloom::UInputDevice device("Keyloom fault probe", phys, {KEY_A}, relative, absolute);
        std::cerr << "expected setup failure at " << expected << " but device was created\n";
        return 1;
    } catch (const keyloom::BackendError& error) {
        const std::string msg = error.what();
        if (msg.find(expected) == std::string::npos) {
            std::cerr << "error message missing '" << expected << "': " << msg << "\n";
            return 1;
        }
        std::cout << "ok   real /dev/uinput ioctl fault at " << expected << ": " << msg << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "unexpected error: " << error.what() << "\n";
        return 1;
    }
}
