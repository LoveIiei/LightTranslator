#include <doctest/doctest.h>

#include "core/Encoding.h"

#include <string>

using namespace std::string_literals;

TEST_CASE("Encoding: empty strings")
{
    CHECK(Encoding::ToUtf8(L"").empty());
    CHECK(Encoding::FromUtf8("").empty());
    const auto strict = Encoding::FromUtf8Strict("");
    REQUIRE(strict.has_value());
    CHECK(strict->empty());
}

TEST_CASE("Encoding: ASCII, CJK and Arabic round-trip")
{
    const std::wstring samples[] = {
        L"Hello, world!",
        L"Grüße aus Köln",
        L"日本語のテキスト",   // Japanese
        L"مرحبا بالعالم", // Arabic
        L"line1\r\nline2\ttab",
    };
    for (const auto& sample : samples) {
        const std::string utf8 = Encoding::ToUtf8(sample);
        const auto back = Encoding::FromUtf8Strict(utf8);
        REQUIRE(back.has_value());
        CHECK(*back == sample);
        CHECK(Encoding::FromUtf8(utf8) == sample);
    }
}

TEST_CASE("Encoding: emoji (surrogate pairs) round-trip")
{
    const std::wstring grin = L"\xD83D\xDE00"; // U+1F600
    const std::string utf8 = Encoding::ToUtf8(grin);
    CHECK(utf8 == "\xF0\x9F\x98\x80");
    const auto back = Encoding::FromUtf8Strict(utf8);
    REQUIRE(back.has_value());
    CHECK(*back == grin);

    const std::wstring mixed = L"ok \xD83D\xDC4D\xD83C\xDFFD done"; // thumbs up + skin tone
    const auto mixedBack = Encoding::FromUtf8Strict(Encoding::ToUtf8(mixed));
    REQUIRE(mixedBack.has_value());
    CHECK(*mixedBack == mixed);
}

TEST_CASE("Encoding: lone surrogates are replaced with U+FFFD when encoding")
{
    const std::string replacement = "\xEF\xBF\xBD";

    CHECK(Encoding::ToUtf8(L"\xD83D") == replacement);                    // lone high surrogate
    CHECK(Encoding::ToUtf8(L"\xDE00") == replacement);                    // lone low surrogate
    CHECK(Encoding::ToUtf8(L"a\xD83D" L"b") == "a" + replacement + "b");  // in the middle of text

    // The result is valid UTF-8 and decodes strictly.
    const auto back = Encoding::FromUtf8Strict(Encoding::ToUtf8(L"a\xD83D" L"b"));
    REQUIRE(back.has_value());
    CHECK(*back == L"a\xFFFD" L"b");
}

TEST_CASE("Encoding: strict decoding rejects invalid UTF-8")
{
    CHECK_FALSE(Encoding::FromUtf8Strict("\xFF").has_value());
    CHECK_FALSE(Encoding::FromUtf8Strict("abc\x80").has_value());          // stray continuation byte
    CHECK_FALSE(Encoding::FromUtf8Strict("\xC0\xAF").has_value());         // overlong '/'
    CHECK_FALSE(Encoding::FromUtf8Strict("\xED\xA0\xBD").has_value());     // UTF-8 encoded surrogate
    CHECK_FALSE(Encoding::FromUtf8Strict("\xF0\x9F\x98").has_value());     // truncated 4-byte sequence
}

TEST_CASE("Encoding: lenient decoding substitutes instead of failing")
{
    const std::wstring decoded = Encoding::FromUtf8("a\xFF" "b");
    CHECK(decoded.front() == L'a');
    CHECK(decoded.back() == L'b');
    CHECK(decoded.find(L'\xFFFD') != std::wstring::npos);
}

TEST_CASE("Encoding: embedded NUL is preserved")
{
    const std::wstring withNul = L"a\0b"s;
    const std::string utf8 = Encoding::ToUtf8(withNul);
    CHECK(utf8.size() == 3);
    const auto back = Encoding::FromUtf8Strict(utf8);
    REQUIRE(back.has_value());
    CHECK(*back == withNul);
}
