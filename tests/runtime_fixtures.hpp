#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backends.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "value.hpp"

namespace keyloom_test {

/// Records every output action so a test can assert on the exact sequence.
struct RecordingBackend : keyloom::OutputBackend {
    std::vector<std::string> log;

    void warp(int x, int y) override {
        log.push_back("warp " + std::to_string(x) + ", " + std::to_string(y));
    }
    void move(int dx, int dy) override {
        log.push_back("move " + std::to_string(dx) + ", " + std::to_string(dy));
    }
    void button(const std::string& name, bool down) override {
        log.push_back("button " + name + (down ? " down" : " up"));
    }
    void key(const std::string& name, bool down) override {
        log.push_back("key " + name + (down ? " down" : " up"));
    }
    void type_text(const std::string& text) override { log.push_back("type " + text); }
    void scroll(int x, int y) override {
        log.push_back("scroll " + std::to_string(x) + ", " + std::to_string(y));
    }
    void close() override { log.push_back("close"); }
    const char* name() const override { return "RecordingBackend"; }
};

/// Evaluate a single expression in a throwaway script and return its value.
inline keyloom::Value eval_expression(const std::string& expression) {
    keyloom::Script script =
        keyloom::parse("loom \"expr\" { when app starts { set result = " + expression + " } }");
    keyloom::Runtime runtime(script, keyloom::Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    return runtime.variables().at("result");
}

}  // namespace keyloom_test
