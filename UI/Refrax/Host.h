/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <Hub/HubClientEndpoint.h>
#include <LibGfx/Size.h>
#include <UI/Refrax/HubLink.h>
#include <UI/Refrax/Wire.h>

namespace Refrax {

// A question the Hub waits on, which Refrax answers.
struct PendingRequest {
    enum class Kind : u8 {
        Alert,
        Confirm,
        Prompt,
        BeforeUnload,
        Permission,
        Navigation,
    };
    Kind kind;
    // The Hub's own number for the question, where it has one.
    u64 answer_id { 0 };
};

// What the host keeps of a page that Refrax shows: the Hub's view behind it, and what the contract's events are
// derived from.
struct Page {
    AK_ALLOC_WITH_KMALLOC;

    // Refrax's number for the page, which every message about it carries.
    u64 id { 0 };
    NonnullRefPtr<HubLink> hub;
    u64 view_id { 0 };
    Hub::ViewState state {};

    Gfx::IntSize size_in_points { 800, 600 };
    double scale { 2.0 };

    // A history traversal that Refrax asked for has yet to commit, so the commit it leads to is a back or forward one.
    bool awaits_history_commit { false };
    // The HTTP status of the response that the navigation under way got, which its finish reports.
    Optional<u16> response_status {};
    // The navigation under way got no response. The engine shows a page saying so, whose own commit and finish are
    // no navigation of Refrax's.
    bool navigation_failed { false };
    bool reported_can_go_back { false };
    bool reported_can_go_forward { false };
    double reported_zoom { 1.0 };
    bool plays_audio { false };
    bool uses_microphone { false };

    // The backing store that the Hub presented last, which goes back once another replaces it.
    Optional<i32> front_backing_store {};
    u64 front_backing_store_page_id { 0 };

    HashMap<u64, PendingRequest> requests {};

    // Refrax asked for the page's renderer to end, so its end is no crash.
    bool termination_was_requested { false };
    // The Compositor has shown a frame of the page, so there is one to take a snapshot of.
    bool has_frame { false };
    Vector<u64> snapshots_awaiting_frame {};
};

class Host {
    AK_MAKE_NONCOPYABLE(Host);
    AK_MAKE_NONMOVABLE(Host);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit Host(NonnullOwnPtr<Wire>);
    ~Host();

    Page* page_for_view(HubLink const&, u64 view_id);

    void emit_event(Page const&, StringView name, JsonObject fields);
    void emit_request(Page&, PendingRequest, StringView name, JsonObject fields);
    void send(StringView name, JsonObject fields) { m_wire->send(name, move(fields)); }

    // The contract's events that follow from a view's state, sent when the state changes what they say.
    void apply_state(Page&, Hub::ViewState);
    void emit_media_changed(Page const&);

    void page_did_show_frame(Page&);
    void hub_died(HubLink&);

    // Writes a frame's pixels to a file under the engine's storage and returns its path.
    ErrorOr<String> write_snapshot_pixels(u64 request_id, ReadonlyBytes);

private:
    void handle_message(String const& name, JsonObject const& fields);

    void handle_start(JsonObject const&);
    void handle_make_page(JsonObject const&);
    void handle_close_page(JsonObject const&);
    void handle_command(Page&, JsonObject const& command);
    void handle_evaluate(Page&, JsonObject const&);
    void handle_snapshot(Page&, JsonObject const&);
    void handle_answer(Page&, JsonObject const&);
    void handle_geometry(Page&, JsonObject const&);
    void handle_mouse(Page&, JsonObject const&);
    void handle_key(Page&, JsonObject const&);
    void handle_edit(Page&, JsonObject const&);
    void handle_remove_profile(JsonObject const&);
    void handle_shutdown();

    ErrorOr<NonnullRefPtr<HubLink>> hub_for_profile(JsonObject const& profile);
    static String profile_key(JsonObject const& profile);
    ErrorOr<ByteString> profile_directory(String const& key) const;
    Hub::UIInfo ui_info() const;
    void close_page(Page&);
    void cancel_pending_dialogs(Page&);
    void take_snapshot(Page&, u64 id);

    // How long a snapshot waits for a page that has shown nothing yet.
    static constexpr auto FIRST_FRAME_WAIT_LIMIT = AK::Duration::from_seconds(3);

    NonnullOwnPtr<Wire> m_wire;
    HashMap<u64, NonnullOwnPtr<Page>> m_pages;
    HashMap<String, NonnullRefPtr<HubLink>> m_hubs;

    ByteString m_storage_directory;
    Vector<String> m_languages;
    Core::AnonymousBuffer m_system_theme;
    u64 m_next_request_id { 1 };
    u64 m_next_input_id { 1 };
    bool m_started { false };
};

}
