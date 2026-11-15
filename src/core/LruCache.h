#pragma once
#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// Least-recently-used cache with an entry cap and a byte cap.
// UI-thread only: no locks. Never persisted.
//
// The full key string is stored with each entry and the map is keyed by it, so a hit always
// compares the complete key (provider + languages + full source text), never just a hash.
template <class Value>
class LruCache {
public:
    using SizeFn = size_t (*)(const Value&);

    LruCache(size_t maxEntries, size_t maxBytes, SizeFn valueBytes)
        : m_maxEntries(maxEntries), m_maxBytes(maxBytes), m_valueBytes(valueBytes) {}

    // Returns the cached value and marks it most recently used, or nullptr.
    const Value* Find(std::wstring_view key)
    {
        const auto it = m_index.find(key);
        if (it == m_index.end()) {
            return nullptr;
        }
        m_entries.splice(m_entries.begin(), m_entries, it->second); // iterators stay valid
        return &it->second->value;
    }

    void Put(std::wstring key, Value value)
    {
        const size_t bytes = EntryBytes(key, value);
        if (const auto it = m_index.find(key); it != m_index.end()) {
            Erase(it->second);
        }
        if (m_maxEntries == 0 || bytes > m_maxBytes) {
            return; // would never fit
        }
        m_entries.push_front(Entry{std::move(key), std::move(value), bytes});
        m_index.emplace(std::wstring_view(m_entries.front().key), m_entries.begin());
        m_bytes += bytes;
        while (m_entries.size() > m_maxEntries || m_bytes > m_maxBytes) {
            Erase(std::prev(m_entries.end()));
        }
    }

    void Clear()
    {
        m_index.clear();
        m_entries.clear();
        m_bytes = 0;
    }

    size_t Size() const { return m_entries.size(); }
    size_t Bytes() const { return m_bytes; }

private:
    struct Entry {
        std::wstring key;
        Value value;
        size_t bytes;
    };
    using Iterator = typename std::list<Entry>::iterator;

    size_t EntryBytes(const std::wstring& key, const Value& value) const
    {
        return key.size() * sizeof(wchar_t) + m_valueBytes(value);
    }

    void Erase(Iterator it)
    {
        m_bytes -= it->bytes;
        m_index.erase(std::wstring_view(it->key));
        m_entries.erase(it);
    }

    size_t m_maxEntries;
    size_t m_maxBytes;
    SizeFn m_valueBytes;
    size_t m_bytes = 0;
    std::list<Entry> m_entries; // front = most recently used; nodes never move, so views into keys stay valid
    std::unordered_map<std::wstring_view, Iterator> m_index;
};
