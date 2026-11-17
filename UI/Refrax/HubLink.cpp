/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <LibCore/MachPort.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/MachBootstrapListener.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <AK/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <UI/Refrax/Host.h>
#include <UI/Refrax/HubLink.h>
#include <sys/wait.h>

namespace Refrax {

// What the Hub gets its end of the connection from. The thread that watches for the Hub to end can outlive connecting.
struct HubBootstrap : public AtomicRefCounted<HubBootstrap> {
    IPC::TransportBootstrapMachServer server;
};

static ErrorOr<ByteString> hub_executable_path()
{
    for (auto const& path : TRY(WebView::get_paths_for_helper_process("Hub"sv))) {
        if (!Core::System::access(path, X_OK).is_error())
            return path;
    }
    return Error::from_string_literal("Unable to find the Hub");
}

ErrorOr<NonnullRefPtr<HubLink>> HubLink::launch(Host& host, String profile_key, Vector<ByteString> arguments, Hub::UIInfo const& ui_info)
{
    auto hub_path = TRY(hub_executable_path());

    IPC::MessageBuffer ui_info_buffer;
    IPC::Encoder encoder { ui_info_buffer };
    TRY(encoder.encode(ui_info));
    arguments.append(ByteString::formatted("--ui-info={}", encode_hex(ui_info_buffer.data().span())));

    // NB: This must answer while the main thread waits for the Hub to initialize, so it answers on its own thread.
    auto hub_bootstrap = adopt_ref(*new HubBootstrap);
    IPC::MachBootstrapListener bootstrap_listener { WebView::mach_server_name_for_process("Ladybird-UI"sv, Core::System::getpid()) };
    if (!bootstrap_listener.is_initialized())
        return Error::from_string_literal("Unable to start the Mach server for the Hub");
    bootstrap_listener.on_bootstrap_request = [hub_bootstrap](IPC::MachBootstrapListener::BootstrapRequest request) {
        if (auto result = hub_bootstrap->server.handle_bootstrap_request(request.pid, move(request.reply_port)); result.is_error())
            dbgln("Unable to bootstrap the Hub: {}", result.error());
    };
    arguments.append(ByteString::formatted("--ui-mach-server={}", bootstrap_listener.server_port_name()));

    Core::ProcessSpawnOptions spawn_options {
        .name = "Hub"sv,
        .executable = hub_path,
        .arguments = arguments,
    };

    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    // The Hub may receive startup messages before it has constructed its transport.
    IPC::TransportMachPort::raise_receive_queue_limit(port_b_recv);

    Optional<Core::Process> spawned_process;
    {
        MutexLocker child_registration_locker(hub_bootstrap->server.child_registration_lock());
        spawned_process = TRY(Core::Process::spawn(spawn_options));
        hub_bootstrap->server.register_child_transport(spawned_process->pid(), IPC::TransportBootstrapMachPorts { move(port_b_recv), move(port_a_send) });
    }
    auto process = spawned_process.release_value();
    auto hub_pid = process.pid();

    // NB: Until the Hub takes its ports, we hold them ourselves, so nothing would tell us of a Hub that ended before
    //     that. Letting go of them then closes the connection, as a socket does when its peer ends. Waiting here also
    //     reaps the Hub once it ends.
    auto hub_exit_watcher = Threading::Thread::construct("HubExitWatcher"sv, [hub_bootstrap, hub_pid]() -> intptr_t {
        int status = 0;
        while (waitpid(hub_pid, &status, 0) < 0 && errno == EINTR)
            ;
        hub_bootstrap->server.unregister_child_transport(hub_pid);
        return 0;
    });
    hub_exit_watcher->start();
    hub_exit_watcher->detach();

    auto transport = make<IPC::Transport>(move(port_a_recv), move(port_b_send));
    auto link = adopt_ref(*new HubLink(host, move(profile_key), move(transport)));
    link->m_hub_pid = hub_pid;

    auto initial_state = link->try_initialize();
    if (initial_state.is_error())
        return Error::from_string_literal("The Hub ended before it initialized");
    link->m_did_initialize = true;

    // NB: Messages to the Hub arrive in order, so this comes before whatever the first page asks for.
    link->async_ui_did_start();
    return link;
}

HubLink::HubLink(Host& host, String profile_key, NonnullOwnPtr<IPC::Transport> transport)
    : HubLinkBase(move(transport))
    , m_host(host)
    , m_profile_key(move(profile_key))
{
}

void HubLink::disconnect()
{
    m_disconnected = true;
    shutdown();
}

void HubLink::die()
{
    // NB: A Hub that ends before answering initialize() is reported by launch(), and one the host let go of is no loss.
    if (!m_did_initialize || m_disconnected)
        return;
    m_host.hub_died(*this);
}

static JsonObject url_fields(URL::URL const& url)
{
    JsonObject fields;
    fields.set("url"sv, url.serialize());
    return fields;
}

void HubLink::view_state_changed(u64 view_id, Hub::ViewState state)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.apply_state(*page, move(state));
}

// The contract's kind of a failure, from the words of the engine's network error.
static StringView failure_kind(StringView error)
{
    auto mentions = [&](StringView words) { return error.contains(words, CaseSensitivity::CaseInsensitive); };
    if (mentions("resolve"sv) || mentions("dns"sv) || mentions("host not found"sv) || mentions("lookup"sv))
        return "cannotFindHost"sv;
    if (mentions("refused"sv) || mentions("connect"sv))
        return "cannotConnectToHost"sv;
    if (mentions("timed out"sv) || mentions("timeout"sv))
        return "timedOut"sv;
    if (mentions("certificate"sv) || mentions("ssl"sv) || mentions("tls"sv))
        return "certificateInvalid"sv;
    if (mentions("reset"sv) || mentions("closed"sv))
        return "connectionLost"sv;
    return "other"sv;
}

void HubLink::view_navigation_response(u64 view_id, Vector<URL::URL> url_list, Optional<u16> status, Optional<String> network_error)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page || url_list.is_empty())
        return;

    for (size_t index = 1; index < url_list.size(); ++index)
        m_host.emit_event(*page, "navigationRedirected"sv, url_fields(url_list[index]));

    page->response_status = status;
    page->navigation_failed = network_error.has_value();
    if (!network_error.has_value())
        return;

    JsonObject failure;
    failure.set("kind"sv, failure_kind(*network_error));
    failure.set("url"sv, url_list.last().serialize());
    failure.set("isProvisional"sv, true);
    failure.set("engineCode"sv, 0);
    failure.set("description"sv, *network_error);
    JsonObject fields;
    fields.set("failure"sv, move(failure));
    m_host.emit_event(*page, "navigationFailed"sv, move(fields));
}

void HubLink::view_navigation_load_start(u64 view_id, URL::URL url)
{
    if (auto* page = m_host.page_for_view(*this, view_id)) {
        page->response_status = {};
        page->navigation_failed = false;
        m_host.emit_event(*page, "navigationStarted"sv, url_fields(url));
        JsonObject progress;
        progress.set("progress"sv, 0.1);
        m_host.emit_event(*page, "progressChanged"sv, move(progress));
    }
}

void HubLink::view_navigation_load_finish(u64 view_id, URL::URL url)
{
    if (auto* page = m_host.page_for_view(*this, view_id)) {
        JsonObject progress;
        progress.set("progress"sv, 1.0);
        m_host.emit_event(*page, "progressChanged"sv, move(progress));
        if (page->navigation_failed)
            return;
        auto fields = url_fields(url);
        if (page->response_status.has_value())
            fields.set("statusCode"sv, *page->response_status);
        m_host.emit_event(*page, "navigationFinished"sv, move(fields));
    }
}

void HubLink::view_top_level_navigation_commit(u64 view_id)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    if (page->navigation_failed)
        return;
    // NB: The Hub pushes the view's state ahead of each of its messages, so the state holds the committed URL.
    auto fields = url_fields(page->state.url);
    fields.set("isBackForward"sv, exchange(page->awaits_history_commit, false));
    m_host.emit_event(*page, "navigationCommitted"sv, move(fields));
}

void HubLink::view_url_change(u64 view_id, URL::URL url)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_event(*page, "urlChanged"sv, url_fields(url));
}

void HubLink::view_title_change(u64 view_id, Utf16String title)
{
    if (auto* page = m_host.page_for_view(*this, view_id)) {
        JsonObject fields;
        fields.set("title"sv, title.to_utf8());
        m_host.emit_event(*page, "titleChanged"sv, move(fields));
    }
}

void HubLink::view_loading_state_change(u64 view_id, bool is_loading)
{
    if (auto* page = m_host.page_for_view(*this, view_id)) {
        JsonObject fields;
        fields.set("isLoading"sv, is_loading);
        m_host.emit_event(*page, "loadingChanged"sv, move(fields));
    }
}

void HubLink::view_link_hover(u64 view_id, URL::URL url)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_event(*page, "hoveredLinkChanged"sv, url_fields(url));
}

void HubLink::view_link_unhover(u64 view_id)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_event(*page, "hoveredLinkChanged"sv, {});
}

void HubLink::view_find_in_page(u64 view_id, size_t current_match_index, Optional<size_t> total_match_count)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    auto matches = total_match_count.value_or(0);
    JsonObject fields;
    fields.set("matches"sv, static_cast<u64>(matches));
    fields.set("activeMatch"sv, static_cast<u64>(matches > 0 ? current_match_index + 1 : 0));
    fields.set("isFinal"sv, true);
    m_host.emit_event(*page, "findResultChanged"sv, move(fields));
}

void HubLink::view_web_content_crashed(u64 view_id, WebView::ViewImplementation::WebContentCrashReason)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject terminated;
    terminated.set("reason"sv, exchange(page->termination_was_requested, false) ? "requestedByBrowser"sv : "crashed"sv);
    JsonObject health;
    health.set("terminated"sv, move(terminated));
    JsonObject fields;
    fields.set("health"sv, move(health));
    m_host.emit_event(*page, "rendererHealthChanged"sv, move(fields));
}

void HubLink::view_tab_indicator_change(u64 view_id, WebView::TabIndicator indicator, bool visible)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    switch (indicator) {
    case WebView::TabIndicator::PlayingAudio:
        page->plays_audio = visible;
        break;
    case WebView::TabIndicator::UsingMicrophone:
        page->uses_microphone = visible;
        break;
    case WebView::TabIndicator::UsingLocation:
        return;
    }
    m_host.emit_media_changed(*page);
}

void HubLink::view_cursor_change(u64 view_id, Gfx::Cursor cursor)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("name"sv, cursor.visit([](Gfx::StandardCursor standard) { return Gfx::standard_cursor_to_string(standard); }, [](Gfx::ImageCursor const&) { return "Arrow"sv; }));
    m_host.send("cursor"sv, move(fields));
}

void HubLink::view_set_remote_layer(u64 view_id, Compositing::RemoteLayerHostHandle host_handle)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("contextID"sv, host_handle.kind == Compositing::RemoteLayerHostKind::CoreAnimationContext ? host_handle.id : 0);
    m_host.send("layer"sv, move(fields));
}

void HubLink::view_did_present_frame_in_remote_layer(u64 view_id, Gfx::IntSize, double)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.page_did_show_frame(*page);
}

void HubLink::view_present_backing_store(u64 view_id, u64 page_id, i32 bitmap_id, Gfx::IntSize, double)
{
    // NB: The page shows through its remote layer. A frame that arrives as a backing store, as before the layer has one,
    //     is shown nowhere, and goes back once the next replaces it so that the page never runs out of them.
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page) {
        async_release_backing_store(page_id, bitmap_id);
        return;
    }
    if (page->front_backing_store.has_value())
        async_release_backing_store(page->front_backing_store_page_id, *page->front_backing_store);
    page->front_backing_store = bitmap_id;
    page->front_backing_store_page_id = page_id;
}

void HubLink::view_did_take_frame_snapshot(u64 view_id, u64 request_id, Optional<Gfx::ShareableBitmap> snapshot)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("id"sv, request_id);
    if (snapshot.has_value() && snapshot->is_valid()) {
        auto const& bitmap = *snapshot->bitmap();
        fields.set("width"sv, bitmap.width());
        fields.set("height"sv, bitmap.height());
        fields.set("bytesPerRow"sv, static_cast<u64>(bitmap.pitch()));
        // NB: A frame is tens of megabytes, so it goes to a file that the client reads and removes, and the message
        //     names the file. The pixels are BGRA with premultiplied alpha, top row first.
        if (auto path = m_host.write_snapshot_pixels(request_id, { bitmap.scanline_u8(0), bitmap.size_in_bytes() }); !path.is_error())
            fields.set("path"sv, path.release_value());
    }
    m_host.send("snapshotTaken"sv, move(fields));
}

void HubLink::view_did_evaluate_script(u64 view_id, u64 request_id, Optional<String> result_json, Optional<String> error)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("id"sv, request_id);
    if (result_json.has_value())
        fields.set("result"sv, result_json.release_value());
    else
        fields.set("error"sv, error.value_or("The script did not finish"_string));
    m_host.send("evaluated"sv, move(fields));
}

void HubLink::view_finish_handling_key_event(u64 view_id, u64 input_id, Web::KeyEvent)
{
    // The page left the key alone, so it is Refrax's to act on, as a menu shortcut for instance.
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("id"sv, input_id);
    m_host.send("keyUnhandled"sv, move(fields));
}

void HubLink::view_release_input(u64 view_id, u64 input_id)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page)
        return;
    JsonObject fields;
    fields.set("page"sv, page->id);
    fields.set("id"sv, input_id);
    m_host.send("keyHandled"sv, move(fields));
}

static JsonObject dialog_fields(StringView kind, Utf16String const& message, Optional<Utf16String> const& default_text, Page const& page)
{
    // Refrax shows at most this much of a dialog's text.
    static constexpr size_t MAXIMUM_DIALOG_TEXT_LENGTH = 2000;
    auto capped = [](Utf16String const& text) {
        auto utf8 = text.to_utf8();
        if (utf8.code_points().length() <= MAXIMUM_DIALOG_TEXT_LENGTH)
            return utf8;
        StringBuilder builder;
        size_t count = 0;
        for (auto code_point : utf8.code_points()) {
            if (count++ == MAXIMUM_DIALOG_TEXT_LENGTH)
                break;
            builder.append_code_point(code_point);
        }
        return builder.to_string_without_validation();
    };

    JsonObject dialog;
    dialog.set("kind"sv, kind);
    dialog.set("message"sv, capped(message));
    if (default_text.has_value())
        dialog.set("defaultText"sv, capped(*default_text));
    dialog.set("origin"sv, page.state.url.origin().serialize());
    JsonObject fields;
    fields.set("dialog"sv, move(dialog));
    return fields;
}

void HubLink::view_request_alert(u64 view_id, Utf16String message)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_request(*page, { .kind = PendingRequest::Kind::Alert }, "javaScriptDialog"sv, dialog_fields("alert"sv, message, {}, *page));
}

void HubLink::view_request_confirm(u64 view_id, Utf16String message)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_request(*page, { .kind = PendingRequest::Kind::Confirm }, "javaScriptDialog"sv, dialog_fields("confirm"sv, message, {}, *page));
}

void HubLink::view_request_prompt(u64 view_id, Utf16String message, Utf16String default_value)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_request(*page, { .kind = PendingRequest::Kind::Prompt }, "javaScriptDialog"sv, dialog_fields("prompt"sv, message, default_value, *page));
}

void HubLink::view_request_before_unload(u64 view_id, u64 answer_id, String)
{
    if (auto* page = m_host.page_for_view(*this, view_id))
        m_host.emit_request(*page, { .kind = PendingRequest::Kind::BeforeUnload, .answer_id = answer_id }, "javaScriptDialog"sv, dialog_fields("beforeUnload"sv, {}, {}, *page));
}

void HubLink::view_request_dismiss_dialog(u64 view_id)
{
    // The page's dialog went away by itself, as when it navigated, so nothing waits on Refrax's answer anymore.
    if (auto* page = m_host.page_for_view(*this, view_id)) {
        page->requests.remove_all_matching([](auto, PendingRequest const& request) {
            return request.kind == PendingRequest::Kind::Alert || request.kind == PendingRequest::Kind::Confirm || request.kind == PendingRequest::Kind::Prompt;
        });
    }
}

void HubLink::view_request_navigation(u64 view_id, u64 answer_id, URL::URL url, bool is_form_submission, Web::HTML::UserNavigationInvolvement user_involvement, Optional<URL::Origin> initiator_origin)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page) {
        async_view_answer(view_id, answer_id, true);
        return;
    }
    auto kind = "other"sv;
    if (is_form_submission)
        kind = "formSubmission"sv;
    else if (user_involvement == Web::HTML::UserNavigationInvolvement::Activation)
        kind = "link"sv;
    auto fields = url_fields(url);
    fields.set("kind"sv, kind);
    if (initiator_origin.has_value())
        fields.set("initiatorOrigin"sv, initiator_origin->serialize());
    m_host.emit_request(*page, { .kind = PendingRequest::Kind::Navigation, .answer_id = answer_id }, "navigation"sv, move(fields));
}

void HubLink::view_request_permission(u64 view_id, u64 answer_id, WebView::PermissionPromptRequest request)
{
    auto* page = m_host.page_for_view(*this, view_id);
    if (!page) {
        async_view_answer_permission_request(view_id, answer_id, Web::Permissions::PromptOutcome::Block);
        return;
    }
    auto kind = [&] {
        switch (request.name) {
        case Web::Permissions::PermissionName::Geolocation:
            return "geolocation"sv;
        case Web::Permissions::PermissionName::Microphone:
            return "microphone"sv;
        case Web::Permissions::PermissionName::Notifications:
            return "notifications"sv;
        }
        VERIFY_NOT_REACHED();
    }();
    JsonObject fields;
    fields.set("kind"sv, kind);
    fields.set("origin"sv, request.top_level_origin.serialize());
    m_host.emit_request(*page, { .kind = PendingRequest::Kind::Permission, .answer_id = answer_id }, "permission"sv, move(fields));
}

void HubLink::view_selected_text(u64, u64, Optional<ByteString> text)
{
    // NB: The host asks for a view's selected text only to copy it.
    if (!text.has_value() || text->is_empty())
        return;
    JsonObject fields;
    fields.set("text"sv, MUST(String::from_byte_string(*text)));
    m_host.send("clipboardWrite"sv, move(fields));
}

void HubLink::app_set_clipboard_text(String text, WebView::Application::ClipboardType)
{
    JsonObject fields;
    fields.set("text"sv, move(text));
    m_host.send("clipboardWrite"sv, move(fields));
}

void HubLink::app_read_clipboard_text(u64 request_id, WebView::Application::ClipboardType)
{
    JsonObject fields;
    fields.set("id"sv, request_id);
    fields.set("profile"sv, m_profile_key);
    m_host.send("clipboardRead"sv, move(fields));
}

}
