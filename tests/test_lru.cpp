#include <doctest/doctest.h>

#include "core/LruCache.h"

#include <string>

namespace {

size_t StringBytes(const std::wstring& value)
{
    return value.size() * sizeof(wchar_t);
}

} // namespace

TEST_CASE("LruCache: find returns what was put")
{
    LruCache<std::wstring> cache(4, 1024, &StringBytes);
    CHECK(cache.Find(L"a") == nullptr);

    cache.Put(L"a", L"alpha");
    cache.Put(L"b", L"beta");
    REQUIRE(cache.Find(L"a") != nullptr);
    CHECK(*cache.Find(L"a") == L"alpha");
    REQUIRE(cache.Find(L"b") != nullptr);
    CHECK(*cache.Find(L"b") == L"beta");
    CHECK(cache.Find(L"c") == nullptr);
    CHECK(cache.Size() == 2);
}

TEST_CASE("LruCache: evicts the least recently used entry at the count cap")
{
    LruCache<std::wstring> cache(3, 1 << 20, &StringBytes);
    cache.Put(L"1", L"one");
    cache.Put(L"2", L"two");
    cache.Put(L"3", L"three");

    REQUIRE(cache.Find(L"1") != nullptr); // "1" becomes most recent; "2" is now the oldest

    cache.Put(L"4", L"four");
    CHECK(cache.Size() == 3);
    CHECK(cache.Find(L"2") == nullptr);
    CHECK(cache.Find(L"1") != nullptr);
    CHECK(cache.Find(L"3") != nullptr);
    CHECK(cache.Find(L"4") != nullptr);
}

TEST_CASE("LruCache: evicts by byte cap")
{
    // Each entry: 1-char key (2 bytes) + 10-char value (20 bytes) = 22 bytes. Cap fits two.
    LruCache<std::wstring> cache(100, 50, &StringBytes);
    cache.Put(L"a", std::wstring(10, L'x'));
    cache.Put(L"b", std::wstring(10, L'y'));
    CHECK(cache.Size() == 2);
    CHECK(cache.Bytes() == 44);

    cache.Put(L"c", std::wstring(10, L'z'));
    CHECK(cache.Size() == 2);
    CHECK(cache.Bytes() == 44);
    CHECK(cache.Find(L"a") == nullptr);
    CHECK(cache.Find(L"b") != nullptr);
    CHECK(cache.Find(L"c") != nullptr);

    // A large entry pushes out as many old ones as needed.
    cache.Put(L"d", std::wstring(20, L'w')); // 42 bytes
    CHECK(cache.Size() == 1);
    CHECK(cache.Find(L"d") != nullptr);
    CHECK(cache.Bytes() == 42);
}

TEST_CASE("LruCache: an entry larger than the byte cap is not stored")
{
    LruCache<std::wstring> cache(10, 32, &StringBytes);
    cache.Put(L"small", L"ok");
    cache.Put(L"huge", std::wstring(100, L'x'));
    CHECK(cache.Find(L"huge") == nullptr);
    CHECK(cache.Find(L"small") != nullptr); // existing entries are untouched
    CHECK(cache.Size() == 1);
}

TEST_CASE("LruCache: putting an existing key replaces the value and the byte count")
{
    LruCache<std::wstring> cache(10, 1024, &StringBytes);
    cache.Put(L"k", L"first");
    const size_t before = cache.Bytes();
    cache.Put(L"k", L"second value");
    CHECK(cache.Size() == 1);
    REQUIRE(cache.Find(L"k") != nullptr);
    CHECK(*cache.Find(L"k") == L"second value");
    CHECK(cache.Bytes() == before + (std::wstring(L"second value").size() - std::wstring(L"first").size()) * sizeof(wchar_t));
}

TEST_CASE("LruCache: keys are compared in full, not by prefix or hash")
{
    LruCache<std::wstring> cache(10, 1 << 20, &StringBytes);
    const std::wstring base(500, L'a');
    cache.Put(base + L"1", L"one");
    cache.Put(base + L"2", L"two");
    REQUIRE(cache.Find(base + L"1") != nullptr);
    CHECK(*cache.Find(base + L"1") == L"one");
    REQUIRE(cache.Find(base + L"2") != nullptr);
    CHECK(*cache.Find(base + L"2") == L"two");
    CHECK(cache.Find(base) == nullptr);
    CHECK(cache.Find(base + L"3") == nullptr);
}

TEST_CASE("LruCache: clear empties everything")
{
    LruCache<std::wstring> cache(10, 1024, &StringBytes);
    cache.Put(L"a", L"1");
    cache.Put(L"b", L"2");
    cache.Clear();
    CHECK(cache.Size() == 0);
    CHECK(cache.Bytes() == 0);
    CHECK(cache.Find(L"a") == nullptr);
    cache.Put(L"a", L"again");
    CHECK(cache.Find(L"a") != nullptr);
}

TEST_CASE("LruCache: stays consistent over many insertions")
{
    LruCache<std::wstring> cache(256, 2 * 1024 * 1024, &StringBytes);
    for (int i = 0; i < 2000; ++i) {
        cache.Put(L"key" + std::to_wstring(i), std::wstring(static_cast<size_t>(i % 50), L'v'));
    }
    CHECK(cache.Size() == 256);
    CHECK(cache.Find(L"key1999") != nullptr);
    CHECK(cache.Find(L"key1744") != nullptr); // the 256th most recent
    CHECK(cache.Find(L"key1743") == nullptr);
}
