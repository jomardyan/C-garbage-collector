#include <iostream>
#include <string>

#include "gc/GC.hpp"

namespace {

struct Message {
    explicit Message(std::string text_value) : text(std::move(text_value)) {}

    gc::gc_ptr<Message> next;
    std::string text;
};

}  // namespace

int main() {
    gc::register_current_thread();

    // gc_root keeps the head of the list alive even if the optimizer keeps it in a register.
    gc::gc_root<Message> head(gc::gc_new<Message>("root"));
    head->next = gc::gc_new<Message>("child");

    gc::collect();

    std::cout << "After collection the rooted list is still alive: "
              << head->text << " -> " << head->next->text << '\n';

    // Once the last exact root is cleared, a later collection may reclaim the objects.
    head.reset();
    gc::collect();
    gc::shutdown();
    return 0;
}