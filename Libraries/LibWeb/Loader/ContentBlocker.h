/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web {

class WEB_API ContentBlocker {
    AK_MAKE_NONCOPYABLE(ContentBlocker);
    AK_MAKE_NONMOVABLE(ContentBlocker);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class ResourceType : u8 {
        Document,
        Font,
        Image,
        Media,
        Object,
        Other,
        Ping,
        Script,
        Stylesheet,
        Subdocument,
        WebSocket,
        XMLHttpRequest,
    };

    static ContentBlocker& the();

    bool has_rules() const { return active_engine() != nullptr; }
    bool has_cosmetic_rules() const
    {
        if (embedder_decides())
            return active_engine() != nullptr;
        return m_has_cosmetic_rules.load(AK::MemoryOrder::memory_order_relaxed);
    }

    // An embedder that filters with an engine of its own hands over the engine's serialized form and its resources.
    // From its first word on, only its engine filters; without one, nothing is filtered.
    bool embedder_decides() const { return m_embedder_decides.load(AK::MemoryOrder::memory_order_relaxed); }
    ErrorOr<void> set_embedder_engine(ReadonlyBytes serialized, ReadonlyBytes resources_json);
    void clear_embedder_engine();

    bool is_filtered(URL::URL const&) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, ResourceType) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);
    ErrorOr<void> set_rules_from_bytes(ReadonlyBytes);

    Utf16String cosmetic_style_sheet_for_url(URL::URL const&) const;
    Utf16String cosmetic_style_sheet_for_url(URL::URL const&, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const;
    bool has_generic_cosmetic_selectors_for_url(URL::URL const&, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const;

    static ResourceType resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode);
    static URL::URL source_url_for_matching(URL::URL const&);

private:
    ContentBlocker();
    ~ContentBlocker();

    // Agents on any thread ask, and the engine is not one that several threads can use at once. The atomics let a check
    // for no rules at all skip the lock.
    mutable Mutex m_mutex;
    Atomic<bool> m_has_cosmetic_rules { false };
    Atomic<void*> m_engine { nullptr };
    Atomic<void*> m_embedder_engine { nullptr };
    Atomic<bool> m_embedder_decides { false };

    void* active_engine() const
    {
        return (embedder_decides() ? m_embedder_engine : m_engine).load(AK::MemoryOrder::memory_order_relaxed);
    }
};

}
