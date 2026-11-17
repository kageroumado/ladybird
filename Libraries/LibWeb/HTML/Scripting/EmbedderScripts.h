/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// A script that the embedder of the engine runs in every document it matches, as a browser runs its own content
// scripts and the user's.
struct EmbedderScript {
    enum class InjectionTime : u8 {
        // Before any script of the document, and before the document has a body.
        DocumentStart,
        // Once the document is parsed, before DOMContentLoaded fires.
        DocumentEnd,
    };

    String id;
    Utf16String source;
    InjectionTime injection_time { InjectionTime::DocumentStart };
    // The embedder's world the script runs in, or none for the page's own.
    Optional<String> world;
    bool main_frame_only { false };
    // WebExtensions match patterns. Without any, the script runs in every document.
    Vector<String> matches;
    Vector<String> excludes;
    // The names under window.webkit.messageHandlers that the script's world may post to the embedder on.
    Vector<String> channels;
};

// The embedder's scripts from their JSON form: an array of objects with the fields of the engine contract's policy.
WEB_API Vector<EmbedderScript> parse_embedder_scripts(JsonValue const&);

// The realms of the embedder's worlds over one window. A world shares the window's DOM with the page and with every
// other world, and none of their JavaScript objects: each has a global object, wrappers and built-ins of its own.
class EmbedderWorlds final : public JS::Cell {
    GC_CELL(EmbedderWorlds, JS::Cell);
    GC_DECLARE_ALLOCATOR(EmbedderWorlds);

public:
    virtual ~EmbedderWorlds() override;

    // The realm of the world with this name, made when first asked for.
    JS::Realm& ensure_world(Window&, String const& name);

    // Lets go of every world, as their wrappers must be before the window's document is destroyed.
    void detach();

    bool page_world_has_channels() const { return m_page_world_has_channels; }
    void set_page_world_has_channels() { m_page_world_has_channels = true; }

private:
    EmbedderWorlds() = default;

    virtual void visit_edges(Cell::Visitor&) override;

    struct World {
        OwnPtr<JS::ExecutionContext> execution_context;
        GC::Ptr<JS::Realm> realm;
        GC::Ptr<Bindings::WrapperWorld> wrapper_world;
        GC::Ptr<WindowProxy> window_proxy;
    };
    HashMap<String, World> m_worlds;
    bool m_page_world_has_channels { false };
};

// Runs a script of the embedder in the window, in the world named or else the page's, whether or not the page's own
// scripts may run. The completion is the script's.
WEB_API JS::Completion run_script_of_embedder(Window&, Optional<String> const& world, Utf16String source, StringView filename);

// The realm a script of the embedder runs in.
WEB_API JS::Realm& realm_for_embedder_world(Window&, Optional<String> const& world);

// Runs the embedder's scripts that match the document and are due at this time.
WEB_API void run_embedder_scripts(DOM::Document&, EmbedderScript::InjectionTime);

// The embedder's answer to a message posted on one of its channels, which settles the promise postMessage() returned.
WEB_API void settle_embedder_message(u64 message_id, Optional<String> const& value_json, Optional<String> const& error);

// Forgets the messages that scripts of the window posted and the embedder has yet to answer.
void forget_embedder_messages_of(Window const&);

// Whether the URL matches a WebExtensions match pattern, such as "*://*.example.com/path/*" or "<all_urls>".
WEB_API bool url_matches_pattern(URL::URL const&, StringView pattern);

}
