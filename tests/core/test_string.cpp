#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <utility>

#include "core/containers/string.h"
#include "core/containers/string_hash.h"

using f4::containers::String;
using f4::containers::StringView;

TEST_CASE("string view ownership and lifetime contract") {
    char backing[] = "engine";

    StringView view(backing);
    CHECK(view.isValid());
    CHECK(view.size() == 6);

    // Mutating the backing through the original storage is visible: the view
    // does not copy.
    backing[0] = 'E';
    CHECK(view.data()[0] == 'E');

    {
        String owned(backing);
        StringView escapeAttempt = owned.view();
        CHECK(escapeAttempt == StringView("Engine"));
        // Lifetime rule: views must not outlive `owned`. Nothing here copies.
    }
}

TEST_CASE("null and empty are distinct states for views") {
    const StringView nullView;
    CHECK(!nullView.isValid());
    CHECK(nullView.isEmpty());
    CHECK(nullView.data() == nullptr);

    const char emptyStorage[1] = {'\0'};
    const StringView emptyView(emptyStorage, 0);
    CHECK(emptyView.isValid()); // points at real memory
    CHECK(emptyView.isEmpty());
    // Equality compares contents: both are zero-length, so they compare equal
    // even though only one points at valid storage.
    CHECK(emptyView == nullView);
}

TEST_CASE("string literal construction, c_str, append") {
    String s("hello");
    CHECK(s.size() == 5);
    CHECK(s == String("hello"));
    CHECK(std::strcmp(s.c_str(), "hello") == 0);
    CHECK(s.c_str()[s.size()] == '\0'); // always null-terminated

    s.append(" world");
    CHECK(s == String("hello world"));

    s += StringView("!");
    CHECK(s == String("hello world!"));

    const String combined = String("a") + StringView("b") + StringView("c");
    CHECK(combined == String("abc"));
}

TEST_CASE("default string is empty but valid for all operations") {
    String s;
    CHECK(s.isEmpty());
    CHECK(s.size() == 0);
    CHECK(std::strcmp(s.c_str(), "") == 0); // never returns nullptr
    CHECK(s.view().isEmpty());

    s.append("x"); // growth path from empty buffer
    CHECK(s == String("x"));
}

TEST_CASE("copy is deep, move steals") {
    String source("payload");

    String copy = source;
    CHECK(copy == source);
    source = String("changed");
    CHECK(copy == String("payload")); // unaffected

    const char* movedFromData = copy.c_str();
    String stolen = std::move(copy);
    CHECK(std::strcmp(stolen.c_str(), "payload") == 0);
    CHECK(copy.isEmpty());          // valid, reusable
    CHECK(copy.c_str() != nullptr); // still null-terminated safe

    // Reassignment after move works.
    copy = String("again");
    CHECK(copy == String("again"));
    (void)movedFromData;
}

TEST_CASE("capacity grows geometrically without shrinking content") {
    String s("a");
    const std::size_t initialCapacity = s.capacity();
    std::size_t expectedSize = 1;
    for (int i = 0; i < 100; ++i) {
        s.append("xy");
        expectedSize += 2;
        CHECK(s.size() == expectedSize);
        CHECK(s.capacity() >= expectedSize + 1); // room for terminator
    }
    CHECK(s.capacity() > initialCapacity);
}

TEST_CASE("view comparisons and hashing agree across types") {
    const String owned("same text");
    const StringView view(owned.c_str(), owned.size());

    CHECK(view == StringView("same text"));
    CHECK(String("other") != owned);

    const std::hash<String> hs{};
    const std::hash<StringView> hv{};
    CHECK(hs(owned) == hv(view)); // shared FNV-1a guarantee
}
