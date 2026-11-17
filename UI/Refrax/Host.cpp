/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Resource.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <UI/Refrax/Host.h>
#include <UI/Refrax/MacKeys.h>

namespace Refrax {

Host::Host(NonnullOwnPtr<Wire> wire)
    : m_wire(move(wire))
{
    m_wire->on_message = [this](String const& name, JsonObject const& fields) {
        handle_message(name, fields);
    };
    // Refrax is gone, and nothing else shows these pages.
    m_wire->on_close = [this] {
        handle_shutdown();
    };
}

Host::~Host() = default;

Page* Host::page_for_view(HubLink const& hub, u64 view_id)
{
    for (auto& entry : m_pages) {
        if (entry.value->hub.ptr() == &hub && entry.value->view_id == view_id)
            return entry.value.ptr();
    }
    return nullptr;
}

void Host::emit_event(Page const& page, StringView name, JsonObject fields)
{
    JsonObject event;
    event.set(name, move(fields));
    JsonObject message;
    message.set("page"sv, page.id);
    message.set("event"sv, move(event));
    m_wire->send("event"sv, move(message));
}

void Host::emit_request(Page& page, PendingRequest pending, StringView name, JsonObject fields)
{
    auto id = m_next_request_id++;
    page.requests.set(id, pending);

    JsonObject request;
    request.set(name, move(fields));
    JsonObject message;
    message.set("page"sv, page.id);
    message.set("id"sv, id);
    message.set("request"sv, move(request));
    m_wire->send("request"sv, move(message));
}

void Host::emit_media_changed(Page const& page)
{
    JsonObject media;
    media.set("isPlayingAudio"sv, page.plays_audio);
    media.set("isAudioMuted"sv, page.state.page_mute_state == Web::HTML::MuteState::Muted);
    media.set("camera"sv, "none"sv);
    media.set("microphone"sv, page.uses_microphone ? "active"sv : "none"sv);
    media.set("screen"sv, "none"sv);
    JsonObject fields;
    fields.set("media"sv, move(media));
    emit_event(page, "mediaChanged"sv, move(fields));
}

void Host::apply_state(Page& page, Hub::ViewState state)
{
    auto mute_state_changed = state.page_mute_state != page.state.page_mute_state;
    page.state = move(state);

    if (page.state.can_navigate_back != page.reported_can_go_back || page.state.can_navigate_forward != page.reported_can_go_forward) {
        page.reported_can_go_back = page.state.can_navigate_back;
        page.reported_can_go_forward = page.state.can_navigate_forward;
        JsonObject fields;
        fields.set("canGoBack"sv, page.reported_can_go_back);
        fields.set("canGoForward"sv, page.reported_can_go_forward);
        emit_event(page, "backForwardChanged"sv, move(fields));
    }

    if (page.state.zoom_level != page.reported_zoom) {
        page.reported_zoom = page.state.zoom_level;
        JsonObject fields;
        fields.set("factor"sv, page.reported_zoom);
        emit_event(page, "zoomChanged"sv, move(fields));
    }

    if (mute_state_changed)
        emit_media_changed(page);
}

ErrorOr<String> Host::write_snapshot_pixels(u64 request_id, ReadonlyBytes pixels)
{
    auto directory = LexicalPath::join(m_storage_directory, "Snapshots"sv).string();
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto path = ByteString::formatted("{}/{}-{}.bgra", directory, Core::System::getpid(), request_id);
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(pixels));
    return String::from_byte_string(path);
}

void Host::hub_died(HubLink& hub)
{
    // Every page of the Hub's profile is gone with it. Refrax hears of an engine that ended, and starts another.
    JsonObject fields;
    fields.set("reason"sv, MUST(String::formatted("The Hub of profile {} ended", hub.profile_key())));
    m_wire->send("terminated"sv, move(fields));
    Core::EventLoop::current().quit(1);
}

void Host::handle_message(String const& name, JsonObject const& fields)
{
    if (name == "start"sv)
        return handle_start(fields);
    if (name == "shutdown"sv)
        return handle_shutdown();
    if (!m_started)
        return;
    if (name == "makePage"sv)
        return handle_make_page(fields);
    if (name == "closePage"sv)
        return handle_close_page(fields);
    if (name == "removeProfile"sv)
        return handle_remove_profile(fields);
    if (name == "clipboardText"sv) {
        auto profile = fields.get_string("profile"sv);
        auto id = fields.get_u64("id"sv);
        if (!profile.has_value() || !id.has_value())
            return;
        if (auto hub = m_hubs.get(*profile); hub.has_value())
            (*hub)->async_answer_clipboard_text(*id, Utf16String::from_utf8(fields.get_string("text"sv).value_or({})));
        return;
    }

    auto page_id = fields.get_u64("page"sv);
    if (!page_id.has_value())
        return;
    auto page = m_pages.get(*page_id);
    if (!page.has_value())
        return;

    if (name == "command"sv) {
        if (auto command = fields.get_object("command"sv); command.has_value())
            handle_command(**page, *command);
    } else if (name == "evaluate"sv) {
        handle_evaluate(**page, fields);
    } else if (name == "snapshot"sv) {
        handle_snapshot(**page, fields);
    } else if (name == "answer"sv) {
        handle_answer(**page, fields);
    } else if (name == "geometry"sv) {
        handle_geometry(**page, fields);
    } else if (name == "mouse"sv) {
        handle_mouse(**page, fields);
    } else if (name == "key"sv) {
        handle_key(**page, fields);
    } else if (name == "edit"sv) {
        handle_edit(**page, fields);
    } else if (name == "focusState"sv) {
        (*page)->hub->async_view_set_has_system_focus((*page)->view_id, fields.get_bool("focused"sv).value_or(false));
    }
}

Hub::UIInfo Host::ui_info() const
{
    Hub::UIInfo info;
    info.interface_color_scheme = Web::CSS::PreferredColorScheme::Light;
    info.supports_system_menu_bar = true;
    info.reports_scroll_momentum = true;
    info.scrolls_elastically = true;
    info.preferred_languages = m_languages;
    return info;
}

void Host::handle_start(JsonObject const& fields)
{
    auto fail = [&](StringView message) {
        JsonObject failure;
        failure.set("message"sv, message);
        m_wire->send("startFailed"sv, move(failure));
    };

    if (m_started)
        return fail("The engine has started already"sv);

    auto storage = fields.get_string("storagePath"sv);
    if (!storage.has_value() || !storage->bytes_as_string_view().starts_with('/'))
        return fail("start names no storage directory"sv);
    m_storage_directory = storage->to_byte_string();

    if (auto languages = fields.get_array("languages"sv); languages.has_value()) {
        languages->for_each([&](JsonValue const& language) {
            if (language.is_string())
                m_languages.append(language.as_string());
        });
    }

    auto theme_file = Core::Resource::load_from_uri("resource://themes/Default.ini"sv);
    if (theme_file.is_error())
        return fail("The engine's resources are missing"sv);
    auto theme = Gfx::load_system_theme(theme_file.value()->filesystem_path().to_byte_string());
    if (theme.is_error())
        return fail("The engine's theme does not load"sv);
    m_system_theme = theme.release_value();

    // The shared profile's Hub starts with the engine, so that a Hub that cannot start is found here, not at a page.
    JsonObject shared;
    shared.set("shared"sv, JsonObject {});
    if (auto hub = hub_for_profile(shared); hub.is_error())
        return fail(MUST(String::formatted("{}", hub.error())));

    m_started = true;
    m_wire->send("started"sv, {});
}

String Host::profile_key(JsonObject const& profile)
{
    auto key_with_id = [&](StringView kind) -> Optional<String> {
        auto fields = profile.get_object(kind);
        if (!fields.has_value())
            return {};
        auto id = fields->get_string("id"sv);
        if (!id.has_value() || id->is_empty())
            return {};
        // NB: The key names a directory, so it holds nothing but what a UUID does.
        for (auto byte : id->bytes()) {
            if (!is_ascii_alphanumeric(byte) && byte != '-')
                return {};
        }
        return MUST(String::formatted("{}-{}", kind, *id));
    };
    if (auto key = key_with_id("isolated"sv); key.has_value())
        return key.release_value();
    if (auto key = key_with_id("ephemeral"sv); key.has_value())
        return key.release_value();
    return "shared"_string;
}

ErrorOr<ByteString> Host::profile_directory(String const& key) const
{
    return LexicalPath::join(m_storage_directory, "Profiles"sv, key).string();
}

ErrorOr<NonnullRefPtr<HubLink>> Host::hub_for_profile(JsonObject const& profile)
{
    auto key = profile_key(profile);
    if (auto hub = m_hubs.get(key); hub.has_value())
        return NonnullRefPtr<HubLink> { **hub };

    Vector<ByteString> arguments;
    if (key.starts_with_bytes("ephemeral-"sv)) {
        arguments.append("--temporary-profile"sv);
    } else {
        auto directory = TRY(profile_directory(key));
        TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
        arguments.append(ByteString::formatted("--profile-path={}", directory));
    }

    auto hub = TRY(HubLink::launch(*this, key, move(arguments), ui_info()));
    m_hubs.set(key, hub);
    return hub;
}

void Host::handle_make_page(JsonObject const& fields)
{
    auto id = fields.get_u64("page"sv);
    auto spec = fields.get_object("spec"sv);
    if (!id.has_value() || !spec.has_value() || m_pages.contains(*id))
        return;

    auto report_failure = [&](StringView message) {
        warnln("Refrax host: page {} was not made: {}", *id, message);
        JsonObject failure;
        failure.set("page"sv, *id);
        failure.set("message"sv, message);
        m_wire->send("pageFailed"sv, move(failure));
    };

    JsonObject shared;
    shared.set("shared"sv, JsonObject {});
    auto profile = spec->get_object("profile"sv);
    auto hub = hub_for_profile(profile.has_value() ? *profile : shared);
    if (hub.is_error())
        return report_failure("The profile's Hub did not start"sv);

    auto scale = fields.get_double_with_precision_loss("scale"sv).value_or(2.0);
    Gfx::IntSize size { fields.get_i32("width"sv).value_or(800), fields.get_i32("height"sv).value_or(600) };

    auto view = hub.value()->try_create_view({
        .is_private = WebView::IsPrivate::No,
        .device_pixel_ratio = scale,
        .maximum_frames_per_second = fields.get_double_with_precision_loss("framesPerSecond"sv).value_or(60.0),
        .display_id = {},
        .screen_shows_high_dynamic_range = false,
        .system_theme = m_system_theme,
        .screen_rects = {},
        .owner_view_id = {},
        .websites_prefer_dark = false,
        .is_tab = true,
    });
    if (view.is_error())
        return report_failure("The Hub made no view"sv);

    auto page = make<Page>(Page { .id = *id, .hub = hub.release_value(), .view_id = view.value().view_id() });
    page->scale = scale;
    page->size_in_points = size;
    auto& hub_link = *page->hub;
    auto view_id = page->view_id;
    auto* page_pointer = page.ptr();
    m_pages.set(*id, move(page));
    apply_state(*page_pointer, view.value().take_state());

    hub_link.async_view_set_viewport(view_id, Web::DevicePixelSize { static_cast<int>(size.width() * scale), static_cast<int>(size.height() * scale) }, scale);
    hub_link.async_view_set_hosts_remote_layer(view_id, true);
    hub_link.async_view_set_decides_navigations(view_id, true);
    hub_link.async_view_set_system_visibility_state(view_id, Web::HTML::VisibilityState::Visible);

    if (auto url_text = spec->get_string("initialURL"sv); url_text.has_value()) {
        if (auto url = URL::Parser::basic_parse(*url_text); url.has_value())
            hub_link.async_view_load(view_id, url.release_value(), Web::Bindings::NavigationHistoryBehavior::Auto);
    }
}

void Host::close_page(Page& page)
{
    page.hub->async_destroy_view(page.view_id);
    m_pages.remove(page.id);
}

void Host::handle_close_page(JsonObject const& fields)
{
    auto id = fields.get_u64("page"sv);
    if (!id.has_value())
        return;
    if (auto page = m_pages.get(*id); page.has_value())
        close_page(**page);
}

void Host::handle_command(Page& page, JsonObject const& command)
{
    auto& hub = *page.hub;
    auto view_id = page.view_id;

    command.for_each_member([&](String const& name, JsonValue const& value) {
        static JsonObject const no_fields;
        auto const& fields = value.is_object() ? value.as_object() : no_fields;

        // NB: A dialog holds its page until answered, so one that Refrax has yet to answer is cancelled before a
        //     navigation that Refrax starts.
        if (name.is_one_of("load"sv, "goBack"sv, "goForward"sv, "reload"sv))
            cancel_pending_dialogs(page);

        if (name == "load"sv) {
            auto request = fields.get_object("request"sv);
            if (!request.has_value())
                return;
            auto url = URL::Parser::basic_parse(request->get_string("url"sv).value_or({}));
            if (!url.has_value())
                return;
            hub.async_view_load(view_id, url.release_value(), Web::Bindings::NavigationHistoryBehavior::Auto);
        } else if (name == "goBack"sv) {
            page.awaits_history_commit = true;
            hub.async_view_traverse_the_history_by_delta(view_id, -1);
        } else if (name == "goForward"sv) {
            page.awaits_history_commit = true;
            hub.async_view_traverse_the_history_by_delta(view_id, 1);
        } else if (name == "reload"sv) {
            hub.async_view_reload(view_id, fields.get_bool("fromOrigin"sv).value_or(false) ? WebView::BypassCache::Yes : WebView::BypassCache::No);
        } else if (name == "setZoom"sv) {
            if (auto factor = fields.get_double_with_precision_loss("factor"sv); factor.has_value())
                hub.async_view_set_zoom(view_id, *factor);
        } else if (name == "terminateRenderer"sv) {
            // NB: The Hub names its processes by numbers of its own, and a view by the process id of its renderer.
            page.termination_was_requested = true;
            for (auto const& process : hub.process_snapshots()) {
                if (process.pid == page.state.web_content_pid && process.type == WebView::ProcessType::WebContent)
                    (void)hub.terminate_process(process.child_id);
            }
        } else if (name == "stopLoading"sv) {
            hub.async_view_stop_loading(view_id);
        } else if (name == "focus"sv) {
            hub.async_view_set_has_system_focus(view_id, true);
        } else if (name == "setAudioMuted"sv) {
            auto wants_muted = fields.get_bool("muted"sv).value_or(false);
            if (wants_muted != (page.state.page_mute_state == Web::HTML::MuteState::Muted))
                hub.async_view_toggle_page_mute_state(view_id);
        } else if (name == "find"sv) {
            auto query = fields.get_object("query"sv);
            if (!query.has_value())
                return;
            if (query->get_bool("findNext"sv).value_or(false)) {
                if (query->get_bool("forward"sv).value_or(true))
                    hub.async_view_find_in_page_next_match(view_id);
                else
                    hub.async_view_find_in_page_previous_match(view_id);
                return;
            }
            auto case_sensitivity = query->get_bool("matchCase"sv).value_or(false) ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
            hub.async_view_find_in_page(view_id, Utf16String::from_utf8(query->get_string("text"sv).value_or({})), case_sensitivity, true);
        } else if (name == "stopFinding"sv) {
            hub.async_view_find_in_page_end(view_id);
        } else if (name == "setVisibility"sv) {
            auto hidden = fields.get_string("visibility"sv).value_or({}) == "hidden"sv;
            hub.async_view_set_system_visibility_state(view_id, hidden ? Web::HTML::VisibilityState::Hidden : Web::HTML::VisibilityState::Visible);
        } else if (name == "notificationClicked"sv) {
            if (auto id = fields.get_u64("id"sv); id.has_value())
                hub.async_notification_clicked(*id);
        } else if (name == "notificationClosed"sv) {
            if (auto id = fields.get_u64("id"sv); id.has_value())
                hub.async_notification_closed(*id, true);
        }
    });
}

void Host::cancel_pending_dialogs(Page& page)
{
    Vector<u64> dialogs;
    for (auto const& request : page.requests) {
        switch (request.value.kind) {
        case PendingRequest::Kind::Alert:
            page.hub->async_view_alert_closed(page.view_id);
            dialogs.append(request.key);
            break;
        case PendingRequest::Kind::Confirm:
            page.hub->async_view_confirm_closed(page.view_id, false);
            dialogs.append(request.key);
            break;
        case PendingRequest::Kind::Prompt:
            page.hub->async_view_prompt_closed(page.view_id, {});
            dialogs.append(request.key);
            break;
        case PendingRequest::Kind::BeforeUnload:
        case PendingRequest::Kind::Permission:
        case PendingRequest::Kind::Navigation:
            break;
        }
    }
    for (auto id : dialogs)
        page.requests.remove(id);
}

void Host::take_snapshot(Page& page, u64 id)
{
    page.hub->async_view_take_frame_snapshot(page.view_id, id, {});
}

void Host::page_did_show_frame(Page& page)
{
    page.has_frame = true;
    auto waiting = move(page.snapshots_awaiting_frame);
    for (auto id : waiting)
        take_snapshot(page, id);
}

void Host::handle_evaluate(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    auto request = fields.get_object("request"sv);
    if (!id.has_value() || !request.has_value())
        return;
    page.hub->async_view_evaluate_script(page.view_id, *id, request->get_string("source"sv).value_or({}), request->get_bool("userGesture"sv).value_or(false));
}

void Host::handle_snapshot(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    if (!id.has_value())
        return;
    if (page.has_frame) {
        take_snapshot(page, *id);
        return;
    }
    // The Compositor has shown nothing of the page yet. Its first frame is on its way, and the snapshot is of that.
    page.snapshots_awaiting_frame.append(*id);
    Core::Timer::create_single_shot(FIRST_FRAME_WAIT_LIMIT, [this, page_id = page.id, snapshot_id = *id] {
        auto page = m_pages.get(page_id);
        if (page.has_value() && (*page)->snapshots_awaiting_frame.remove_first_matching([&](auto waiting) { return waiting == snapshot_id; }))
            take_snapshot(**page, snapshot_id);
    })->start();
}

void Host::handle_answer(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    auto answer = fields.get_object("answer"sv);
    if (!id.has_value() || !answer.has_value())
        return;
    auto pending = page.requests.take(*id);
    if (!pending.has_value())
        return;

    auto& hub = *page.hub;
    auto confirm = answer->get_object("confirm"sv);
    auto accepted = confirm.has_value() || answer->has("allow"sv);

    switch (pending->kind) {
    case PendingRequest::Kind::Alert:
        hub.async_view_alert_closed(page.view_id);
        break;
    case PendingRequest::Kind::Confirm:
        hub.async_view_confirm_closed(page.view_id, accepted);
        break;
    case PendingRequest::Kind::Prompt: {
        Optional<Utf16String> response;
        if (confirm.has_value())
            response = Utf16String::from_utf8(confirm->get_string("text"sv).value_or({}));
        hub.async_view_prompt_closed(page.view_id, move(response));
        break;
    }
    case PendingRequest::Kind::BeforeUnload:
    case PendingRequest::Kind::Navigation:
        hub.async_view_answer(page.view_id, pending->answer_id, accepted);
        break;
    case PendingRequest::Kind::Permission:
        // NB: Refrax keeps every permission decision itself, so the engine is only ever told about this one request.
        hub.async_view_answer_permission_request(page.view_id, pending->answer_id, accepted ? Web::Permissions::PromptOutcome::AllowOnce : Web::Permissions::PromptOutcome::Block);
        break;
    }
}

void Host::handle_geometry(Page& page, JsonObject const& fields)
{
    page.scale = fields.get_double_with_precision_loss("scale"sv).value_or(page.scale);
    page.size_in_points = { fields.get_i32("width"sv).value_or(page.size_in_points.width()), fields.get_i32("height"sv).value_or(page.size_in_points.height()) };
    Web::DevicePixelSize size { static_cast<int>(page.size_in_points.width() * page.scale), static_cast<int>(page.size_in_points.height() * page.scale) };
    page.hub->async_view_set_viewport(page.view_id, size, page.scale);
}

// The wire's modifier bits: shift, control, option, command and the numeric keypad, from the lowest up.
static Web::UIEvents::KeyModifier modifiers_from_wire(u32 bits)
{
    auto modifiers = Web::UIEvents::Mod_None;
    if (bits & 1)
        modifiers |= Web::UIEvents::Mod_Shift;
    if (bits & 2)
        modifiers |= Web::UIEvents::Mod_Ctrl;
    if (bits & 4)
        modifiers |= Web::UIEvents::Mod_Alt;
    if (bits & 8)
        modifiers |= Web::UIEvents::Mod_Super;
    if (bits & 16)
        modifiers |= Web::UIEvents::Mod_Keypad;
    return modifiers;
}

void Host::handle_mouse(Page& page, JsonObject const& fields)
{
    auto type_name = fields.get_string("type"sv).value_or({});
    Web::MouseEvent::Type type;
    if (type_name == "down"sv)
        type = Web::MouseEvent::Type::MouseDown;
    else if (type_name == "up"sv)
        type = Web::MouseEvent::Type::MouseUp;
    else if (type_name == "move"sv)
        type = Web::MouseEvent::Type::MouseMove;
    else if (type_name == "leave"sv)
        type = Web::MouseEvent::Type::MouseLeave;
    else if (type_name == "wheel"sv)
        type = Web::MouseEvent::Type::MouseWheel;
    else
        return;

    auto number = [&](StringView key) { return fields.get_double_with_precision_loss(key).value_or(0); };
    auto device_point = [&](StringView x, StringView y) {
        return Web::DevicePixelPoint { static_cast<int>(number(x) * page.scale), static_cast<int>(number(y) * page.scale) };
    };

    // NB: The wire's buttons are the bits of Web::UIEvents::MouseButton: primary 1, secondary 2, middle 4.
    auto button = static_cast<Web::UIEvents::MouseButton>(fields.get_u32("button"sv).value_or(0) & 0x1f);
    auto buttons = static_cast<Web::UIEvents::MouseButton>(fields.get_u32("buttons"sv).value_or(0) & 0x1f);
    if (button == Web::UIEvents::MouseButton::None && (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp))
        return;

    Web::MouseEvent event {
        .type = type,
        .position = device_point("x"sv, "y"sv),
        .screen_position = device_point("screenX"sv, "screenY"sv),
        .button = button,
        .buttons = buttons,
        .modifiers = modifiers_from_wire(fields.get_u32("modifiers"sv).value_or(0)),
        .click_count = fields.get_i32("clickCount"sv).value_or(0),
    };

    if (type == Web::MouseEvent::Type::MouseWheel) {
        event.wheel_delta_precision = fields.get_bool("precise"sv).value_or(false) ? Web::WheelDeltaPrecision::Precise : Web::WheelDeltaPrecision::Discrete;
        event.wheel_delta_x = number("deltaX"sv);
        event.wheel_delta_y = number("deltaY"sv);
        event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, event.wheel_delta_precision);
        event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, event.wheel_delta_precision);
        auto phase = fields.get_string("phase"sv).value_or({});
        if (phase == "ongoing"sv)
            event.scroll_gesture_phase = Web::ScrollGesturePhase::Ongoing;
        else if (phase == "momentum"sv)
            event.scroll_gesture_phase = Web::ScrollGesturePhase::Momentum;
        else if (phase == "ended"sv)
            event.scroll_gesture_phase = Web::ScrollGesturePhase::Ended;
    }

    page.hub->async_view_enqueue_mouse_event(page.view_id, m_next_input_id++, move(event));
}

void Host::handle_key(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    if (!id.has_value())
        return;

    auto is_down = fields.get_string("type"sv).value_or({}) == "down"sv;
    auto modifiers = modifiers_from_wire(fields.get_u32("modifiers"sv).value_or(0));

    // The key is the one its virtual key code names, or else the one that types its character.
    auto characters = fields.get_string("characters"sv).value_or({});
    auto unmodified = fields.get_string("charactersIgnoringModifiers"sv).value_or({});
    auto first_code_point = [](String const& text) -> u32 {
        auto code_points = text.code_points();
        return code_points.is_empty() ? 0 : *code_points.begin();
    };

    auto key = key_code_from_mac_virtual_key(fields.get_u32("keyCode"sv).value_or(0xffff));
    auto code_point = first_code_point(characters);
    if (key == Web::UIEvents::KeyCode::Key_Invalid)
        key = Web::UIEvents::code_point_to_key_code(first_code_point(unmodified.is_empty() ? characters : unmodified));
    if (key == Web::UIEvents::KeyCode::Key_Return)
        code_point = '\n';

    // NB: AppKit reports the arrows, the function keys and their like as characters of a private-use area. They type
    //     nothing, and neither does a key held with Command or Control.
    auto types_text = code_point != 0 && !(code_point >= 0xF700 && code_point <= 0xF8FF) && code_point != 0x7F
        && !(modifiers & (Web::UIEvents::Mod_Super | Web::UIEvents::Mod_Ctrl));
    if (!types_text && key != Web::UIEvents::KeyCode::Key_Return && key != Web::UIEvents::KeyCode::Key_Tab)
        code_point = (modifiers & (Web::UIEvents::Mod_Super | Web::UIEvents::Mod_Ctrl)) ? code_point : 0;

    Web::KeyEvent event {
        .type = is_down ? Web::KeyEvent::Type::KeyDown : Web::KeyEvent::Type::KeyUp,
        .key = key,
        .modifiers = modifiers,
        .code_point = code_point,
        .repeat = fields.get_bool("repeat"sv).value_or(false),
        .should_insert_text = is_down && types_text,
    };

    if (auto commands = fields.get_array("commands"sv); is_down && commands.has_value()) {
        commands->for_each([&](JsonValue const& selector) {
            if (!selector.is_string())
                return;
            if (auto command = text_editing_command_from_selector(selector.as_string()); command.has_value())
                event.editing_commands.append(command.release_value());
        });
        event.native_editing_binding_resolved = true;
    }

    page.hub->async_view_enqueue_key_event(page.view_id, *id, move(event));
}

void Host::handle_edit(Page& page, JsonObject const& fields)
{
    auto action = fields.get_string("action"sv).value_or({});
    auto& hub = *page.hub;
    if (action == "selectAll"sv)
        hub.async_view_select_all(page.view_id);
    else if (action == "copy"sv)
        hub.async_view_request_selected_text(page.view_id, m_next_request_id++);
    else if (action == "paste"sv)
        hub.async_view_paste(page.view_id, Web::Clipboard::PasteMode::AllMimeTypes);
    else if (action == "cut"sv)
        hub.async_view_cut_selection(page.view_id);
    else if (action == "undo"sv)
        hub.async_view_undo(page.view_id);
    else if (action == "redo"sv)
        hub.async_view_redo(page.view_id);
}

void Host::handle_remove_profile(JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    auto profile = fields.get_object("profile"sv);
    if (!id.has_value() || !profile.has_value())
        return;
    auto key = profile_key(*profile);

    Vector<u64> pages_of_profile;
    for (auto const& entry : m_pages) {
        if (entry.value->hub->profile_key() == key)
            pages_of_profile.append(entry.key);
    }
    for (auto page_id : pages_of_profile)
        m_pages.remove(page_id);

    // NB: A Hub holds its profile's databases open until it ends, so its directory goes only once it has.
    if (auto hub = m_hubs.take(key); hub.has_value()) {
        auto pid = (*hub)->hub_pid();
        (*hub)->disconnect();
        for (int attempt = 0; attempt < 200 && ::kill(pid, 0) == 0; ++attempt)
            usleep(25'000);
    }
    if (!key.starts_with_bytes("ephemeral-"sv)) {
        if (auto directory = profile_directory(key); !directory.is_error())
            (void)FileSystem::remove(directory.value(), FileSystem::RecursionMode::Allowed);
    }

    JsonObject removed;
    removed.set("id"sv, *id);
    m_wire->send("profileRemoved"sv, move(removed));
}

void Host::handle_shutdown()
{
    m_pages.clear();
    auto hubs = move(m_hubs);
    for (auto& hub : hubs)
        hub.value->disconnect();
    Core::EventLoop::current().quit(0);
}

}
