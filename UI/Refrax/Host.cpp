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

void Host::run_after(AK::Duration delay, Function<void()> function)
{
    auto id = m_next_timer_id++;
    auto timer = Core::Timer::create_single_shot(delay, [this, id, function = move(function)] {
        function();
        // NB: The timer is running this very function, so it goes once the event loop has it back.
        Core::deferred_invoke([this, id] { m_timers.remove(id); });
    });
    m_timers.set(id, timer);
    timer->start();
}

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
    if (name == "policy"sv) {
        if (auto update = fields.get_object("update"sv); update.has_value())
            handle_policy(*update);
        return;
    }
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
    } else if (name == "accessibility"sv) {
        if (fields.get_bool("enabled"sv).value_or(false) && !(*page)->accessibility_is_wanted) {
            (*page)->accessibility_is_wanted = true;
            (*page)->hub->async_view_request_accessibility_tree((*page)->view_id);
        }
    } else if (name == "accessibilityAction"sv) {
        auto node = fields.get_i64("node"sv);
        auto action = fields.get_string("action"sv);
        if (node.has_value() && action.has_value() && action->is_one_of("press"sv, "focus"sv))
            (*page)->hub->async_view_perform_accessibility_action((*page)->view_id, *node, *action);
    } else if (name == "requestLog"sv) {
        (*page)->hub->async_view_request_content_blocking_log((*page)->view_id, fields.get_u64("id"sv).value_or(0), fields.get_u64("since"sv).value_or(0));
    } else if (name == "answer"sv) {
        handle_answer(**page, fields);
    } else if (name == "scriptReply"sv) {
        handle_script_reply(**page, fields);
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

    // The engine bundle carries the script that applies procedural filters to a page.
    if (auto support = fields.get_string("supportDirectory"sv); support.has_value() && support->bytes_as_string_view().starts_with('/')) {
        if (auto file = Core::File::open(ByteString::formatted("{}/procedural.js", *support), Core::File::OpenMode::Read); !file.is_error()) {
            if (auto source = file.value()->read_until_eof(); !source.is_error())
                m_procedural_runner = String::from_utf8(source.value()).release_value_but_fixme_should_propagate_errors();
        }
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
    // Refrax decides which windows a page may open and where its downloads go.
    hub->async_set_blocks_pop_ups(false);
    if (!m_scripts_json.is_empty())
        hub->async_set_embedder_scripts(m_scripts_json);
    if (!m_site_settings_json.is_empty())
        hub->async_set_embedder_site_settings(m_site_settings_json);
    // Refrax holds every notification decision, from before its first policy on.
    hub->async_set_embedder_notification_policy(m_notification_policy_json.is_empty() ? "{}"_string : m_notification_policy_json);
    if (!m_procedural_runner.is_empty())
        hub->async_set_embedder_procedural_runner(m_procedural_runner);
    if (!m_content_blocking_policy_json.is_empty())
        hub->async_set_embedder_content_blocking(m_content_blocking_policy_json);
    hub->async_set_download_staging_directory(download_staging_directory(key));
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
    // A download that Refrax has yet to place has no one left to ask.
    Vector<u64> downloads_of_page;
    for (auto const& download : m_downloads) {
        if (download.page_id == page.id && !download.destination.has_value())
            downloads_of_page.append(download.id);
    }
    for (auto id : downloads_of_page)
        abandon_download(id);
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
            Vector<HTTP::Header> headers;
            if (auto wanted = request->get_object("headers"sv); wanted.has_value()) {
                wanted->for_each_member([&](auto const& header_name, JsonValue const& value) {
                    // NB: A header is one line. A name or value that would start another is left out.
                    auto is_one_line = [](StringView text) { return !text.contains('\r') && !text.contains('\n'); };
                    if (value.is_string() && !header_name.is_empty() && is_one_line(header_name) && is_one_line(value.as_string()))
                        headers.append({ header_name.to_byte_string(), value.as_string().to_byte_string() });
                });
            }
            if (headers.is_empty())
                hub.async_view_load(view_id, url.release_value(), Web::Bindings::NavigationHistoryBehavior::Auto);
            else
                hub.async_view_load_with_headers(view_id, url.release_value(), move(headers));
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
        } else if (name == "setMediaSuspended"sv) {
            hub.async_view_set_media_suspended(view_id, fields.get_bool("suspended"sv).value_or(false));
        } else if (name == "find"sv) {
            if (auto query = fields.get_object("query"sv); query.has_value())
                start_finding(page, *query);
        } else if (name == "stopFinding"sv) {
            stop_finding(page);
        } else if (name == "cancelDownload"sv) {
            if (auto id = fields.get_u64("id"sv); id.has_value()) {
                abandon_download(*id);
            }
        } else if (name == "setVisibility"sv) {
            auto hidden = fields.get_string("visibility"sv).value_or({}) == "hidden"sv;
            hub.async_view_set_system_visibility_state(view_id, hidden ? Web::HTML::VisibilityState::Hidden : Web::HTML::VisibilityState::Visible);
        } else if (name == "notificationClicked"sv || name == "notificationClosed"sv) {
            // NB: The notification is gone from the screen either way; a click reaches the page before its close.
            auto id = fields.get_string("id"sv);
            auto shown = id.has_value() ? m_notifications.take(*id) : Optional<ShownNotification> {};
            if (shown.has_value()) {
                if (name == "notificationClicked"sv)
                    shown->hub->async_notification_clicked(shown->hub_notification_id);
                shown->hub->async_notification_closed(shown->hub_notification_id, true);
            }
        }
    });
}

void Host::stop_finding(Page& page)
{
    page.find_is_active = false;
    page.hub->async_view_find_in_page_end(page.view_id);
    // NB: The engine keeps the search it ended, and would start the next one from the match it left selected. A search
    //     for nothing forgets it, and its result of no matches is none of Refrax's.
    page.find_results_to_skip++;
    page.hub->async_view_find_in_page(page.view_id, Utf16String {}, CaseSensitivity::CaseInsensitive, false);
}

void Host::start_finding(Page& page, JsonObject const& query)
{
    auto& hub = *page.hub;
    auto text = query.get_string("text"sv).value_or({});

    // An empty search finds nothing and ends the one under way.
    if (text.is_empty()) {
        stop_finding(page);
        JsonObject fields;
        fields.set("matches"sv, 0);
        fields.set("activeMatch"sv, 0);
        fields.set("isFinal"sv, true);
        emit_event(page, "findResultChanged"sv, move(fields));
        return;
    }

    // NB: A search that was stopped is over, so asking for its next match starts it again from the first.
    if (query.get_bool("findNext"sv).value_or(false) && page.find_is_active && page.find_text == text) {
        if (query.get_bool("forward"sv).value_or(true))
            hub.async_view_find_in_page_next_match(page.view_id);
        else
            hub.async_view_find_in_page_previous_match(page.view_id);
        return;
    }

    page.find_is_active = true;
    page.find_text = text;
    auto case_sensitivity = query.get_bool("matchCase"sv).value_or(false) ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
    hub.async_view_find_in_page(page.view_id, Utf16String::from_utf8(text), case_sensitivity, true);
}

void Host::popup_was_requested(HubLink& hub, u64 opener_view_id, u64 new_view_id, StringView disposition, bool has_user_activation)
{
    auto* opener = page_for_view(hub, opener_view_id);
    if (!opener) {
        hub.async_destroy_view(new_view_id);
        return;
    }
    m_pending_popups.append({ .hub = hub, .view_id = new_view_id, .opener_page_id = opener->id, .disposition = disposition, .has_user_activation = has_user_activation });

    // A window opened without a URL never navigates. It has nothing for Refrax to open, and goes after a while.
    run_after(POPUP_NAVIGATION_WAIT_LIMIT, [this, hub = NonnullRefPtr<HubLink> { hub }, new_view_id] {
        if (m_pending_popups.remove_first_matching([&](auto const& popup) { return popup.hub.ptr() == hub.ptr() && popup.view_id == new_view_id; }))
            hub->async_destroy_view(new_view_id);
    });
}

bool Host::popup_did_start_navigating(HubLink& hub, u64 view_id, URL::URL const& url)
{
    auto index = m_pending_popups.find_first_index_if([&](auto const& popup) { return popup.hub.ptr() == &hub && popup.view_id == view_id; });
    if (!index.has_value())
        return false;
    auto popup = m_pending_popups.take(*index);
    hub.async_destroy_view(view_id);

    auto opener = m_pages.get(popup.opener_page_id);
    if (!opener.has_value())
        return true;
    JsonObject fields;
    fields.set("url"sv, url.serialize());
    fields.set("disposition"sv, popup.disposition);
    fields.set("userGesture"sv, popup.has_user_activation);
    fields.set("isNewWindowRequest"sv, true);
    emit_request(**opener, { .kind = PendingRequest::Kind::OpenURL }, "openURL"sv, move(fields));
    return true;
}

ByteString Host::download_staging_directory(String const& profile_key) const
{
    return LexicalPath::join(m_storage_directory, "Downloads"sv, profile_key).string();
}

StagedDownload* Host::staged_download(u64 id)
{
    for (auto& download : m_downloads) {
        if (download.id == id)
            return &download;
    }
    return nullptr;
}

void Host::forget_download(u64 id)
{
    m_downloads.remove_first_matching([&](auto const& download) { return download.id == id; });
}

// Stops the download and removes what it wrote. The Hub's report of the cancelled download finds nothing left of it.
void Host::abandon_download(u64 id)
{
    auto* download = staged_download(id);
    if (!download)
        return;
    download->hub->async_cancel_download(download->hub_download_id);
    (void)FileSystem::remove(download->staged_path, FileSystem::RecursionMode::Disallowed);
    forget_download(id);
}

void Host::download_was_added(HubLink& hub, Hub::DownloadState const& state)
{
    // Only the downloads staged for Refrax are its to place.
    if (!state.destination.starts_with(download_staging_directory(hub.profile_key())))
        return;

    auto id = m_next_download_id++;
    m_downloads.append({ .id = id, .hub = hub, .hub_download_id = state.id, .staged_path = state.destination, .url = state.url, .total_size = state.total_size });

    // NB: Refrax asks the user about a download in the page that started it. One that no view claims has no page to
    //     ask in, and is never attributed to a page by guessing.
    run_after(DOWNLOAD_ATTRIBUTION_WAIT_LIMIT, [this, id] {
        if (auto* download = staged_download(id); download && !download->page_id.has_value())
            abandon_download(id);
    });

    download_was_updated(hub, state);
}

void Host::download_was_started_by_view(HubLink& hub, u64 hub_download_id, u64 view_id)
{
    auto index = m_downloads.find_first_index_if([&](auto const& download) { return download.hub.ptr() == &hub && download.hub_download_id == hub_download_id; });
    if (!index.has_value())
        return;
    auto& download = m_downloads[*index];
    auto* page = page_for_view(hub, view_id);
    if (!page || download.page_id.has_value()) {
        if (!page)
            abandon_download(download.id);
        return;
    }
    download.page_id = page->id;

    // The staged file's name is the suggested one after the download's number in the staging directory.
    LexicalPath staged_path { download.staged_path };
    auto staged_name = staged_path.basename();
    auto suggested_name = staged_name.find('-').map([&](auto index) { return staged_name.substring_view(index + 1); }).value_or(staged_name);

    JsonObject fields;
    fields.set("id"sv, download.id);
    fields.set("url"sv, download.url.serialize());
    fields.set("suggestedFilename"sv, suggested_name);
    if (download.total_size.has_value())
        fields.set("totalBytes"sv, *download.total_size);
    emit_request(*page, { .kind = PendingRequest::Kind::Download, .answer_id = download.id }, "download"sv, move(fields));
}

void Host::download_was_updated(HubLink& hub, Hub::DownloadState const& state)
{
    auto index = m_downloads.find_first_index_if([&](auto const& download) { return download.hub.ptr() == &hub && download.hub_download_id == state.id; });
    if (!index.has_value())
        return;
    auto& download = m_downloads[*index];
    auto page = download.page_id.has_value() ? m_pages.get(*download.page_id) : Optional<Page*> {};

    using Status = WebView::FileDownloader::DownloadStatus;
    switch (state.status) {
    case Status::InProgress:
    case Status::Paused:
        if (page.has_value() && state.downloaded_size != download.reported_size) {
            download.reported_size = state.downloaded_size;
            JsonObject fields;
            fields.set("id"sv, download.id);
            fields.set("receivedBytes"sv, state.downloaded_size);
            if (state.total_size.has_value())
                fields.set("totalBytes"sv, *state.total_size);
            emit_event(**page, "downloadProgressed"sv, move(fields));
        }
        break;
    case Status::Completed:
        download.is_complete = true;
        place_download_if_ready(download);
        break;
    case Status::Canceled:
    case Status::Failed: {
        if (page.has_value()) {
            JsonObject fields;
            fields.set("id"sv, download.id);
            fields.set("reason"sv, state.error.has_value() ? *state.error : "The download was cancelled"_string);
            emit_event(**page, "downloadFailed"sv, move(fields));
        }
        (void)FileSystem::remove(download.staged_path, FileSystem::RecursionMode::Disallowed);
        forget_download(download.id);
        break;
    }
    }
}

// A download is placed once it is whole and Refrax has said where it goes, whichever comes last.
void Host::place_download_if_ready(StagedDownload& download)
{
    if (!download.is_complete || !download.destination.has_value())
        return;

    auto moved = FileSystem::move_file(*download.destination, download.staged_path, FileSystem::PreserveMode::Nothing);
    if (auto page = download.page_id.has_value() ? m_pages.get(*download.page_id) : Optional<Page*> {}; page.has_value()) {
        JsonObject fields;
        fields.set("id"sv, download.id);
        if (moved.is_error()) {
            fields.set("reason"sv, MUST(String::formatted("The file could not be moved into place: {}", moved.error())));
            emit_event(**page, "downloadFailed"sv, move(fields));
        } else {
            emit_event(**page, "downloadFinished"sv, move(fields));
        }
    }
    if (moved.is_error())
        (void)FileSystem::remove(download.staged_path, FileSystem::RecursionMode::Disallowed);
    forget_download(download.id);
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
        case PendingRequest::Kind::OpenURL:
        case PendingRequest::Kind::Download:
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

// The name of the isolated world that the contract's `world` names, or none for the page's own.
static Optional<String> world_name_from_contract(Optional<JsonObject const&> world)
{
    if (!world.has_value())
        return {};
    auto isolated = world->get_object("isolated"sv);
    if (!isolated.has_value())
        return {};
    auto name = isolated->get_string("name"sv);
    if (!name.has_value() || name->is_empty())
        return {};
    return *name;
}

void Host::handle_policy(JsonObject const& update)
{
    // The scripts Refrax injects go to every Hub, and to the ones started later.
    if (auto scripts = update.get_object("scripts"sv); scripts.has_value()) {
        auto list = scripts->get_array("scripts"sv);
        m_scripts_json = list.has_value() ? list->serialized() : "[]"_string;
        for (auto& hub : m_hubs)
            hub.value->async_set_embedder_scripts(m_scripts_json);
    }

    if (auto site_settings = update.get_object("siteSettings"sv); site_settings.has_value()) {
        auto policy = site_settings->get_object("policy"sv);
        m_site_settings_json = policy.has_value() ? policy->serialized() : "{}"_string;
        for (auto& hub : m_hubs)
            hub.value->async_set_embedder_site_settings(m_site_settings_json);
    }

    if (auto blocking = update.get_object("contentBlocking"sv); blocking.has_value()) {
        auto policy = blocking->get_object("policy"sv);
        m_content_blocking_policy_json = policy.has_value() ? policy->serialized() : "{}"_string;
        for (auto& hub : m_hubs)
            hub.value->async_set_embedder_content_blocking(m_content_blocking_policy_json);

        // NB: Every Hub reads the engine as this does, and its pages' loads follow the policy on its connection, so
        //     what this says of the engine holds for each page Refrax loads from here on.
        JsonObject status;
        if (!policy.has_value() || !policy->get_bool("isEnabled"sv).value_or(false)) {
            status.set("status"sv, "unfiltered"sv);
        } else if (auto engine = WebView::Application::read_embedder_blocking_engine(*policy); !engine.is_error()) {
            m_blocking_engine_sha256 = engine.value().sha256;
            status.set("status"sv, "applied"sv);
            status.set("sha256"sv, engine.value().sha256);
        } else {
            if (!WebView::Application::reads_embedder_blocking_engine_format(*policy))
                m_blocking_engine_sha256.clear();
            status.set("status"sv, m_blocking_engine_sha256.has_value() ? "applied"sv : "unfiltered"sv);
            if (m_blocking_engine_sha256.has_value())
                status.set("sha256"sv, *m_blocking_engine_sha256);
            status.set("reason"sv, engine.error());
        }
        JsonObject event;
        event.set("contentBlockingStatus"sv, move(status));
        JsonObject message;
        message.set("event"sv, move(event));
        send("engineEvent"sv, move(message));
    }

    if (auto notifications = update.get_object("notifications"sv); notifications.has_value()) {
        auto policy = notifications->get_object("policy"sv);
        m_notification_policy_json = policy.has_value() ? policy->serialized() : "{}"_string;
        for (auto& hub : m_hubs)
            hub.value->async_set_embedder_notification_policy(m_notification_policy_json);
    }
}

// AppKit's role for a node of the engine's accessibility tree, which names roles as ARIA does.
static StringView appkit_accessibility_role(StringView role)
{
    if (role == "document"sv)
        return "AXWebArea"sv;
    if (role == "text leaf"sv)
        return "AXStaticText"sv;
    if (role == "button"sv)
        return "AXButton"sv;
    if (role == "link"sv)
        return "AXLink"sv;
    if (role == "heading"sv)
        return "AXHeading"sv;
    if (role == "textbox"sv || role == "searchbox"sv)
        return "AXTextField"sv;
    if (role == "checkbox"sv || role == "switch"sv)
        return "AXCheckBox"sv;
    if (role == "radio"sv || role == "tab"sv)
        return "AXRadioButton"sv;
    if (role == "combobox"sv)
        return "AXComboBox"sv;
    if (role == "list"sv || role == "listbox"sv)
        return "AXList"sv;
    if (role == "table"sv || role == "grid"sv || role == "treegrid"sv)
        return "AXTable"sv;
    if (role == "row"sv)
        return "AXRow"sv;
    if (role == "cell"sv || role == "gridcell"sv || role == "columnheader"sv || role == "rowheader"sv)
        return "AXCell"sv;
    if (role == "img"sv || role == "image"sv)
        return "AXImage"sv;
    if (role == "progressbar"sv)
        return "AXProgressIndicator"sv;
    if (role == "slider"sv)
        return "AXSlider"sv;
    if (role == "tablist"sv)
        return "AXTabGroup"sv;
    if (role == "menu"sv)
        return "AXMenu"sv;
    if (role == "menuitem"sv)
        return "AXMenuItem"sv;
    if (role == "menubar"sv)
        return "AXMenuBar"sv;
    if (role == "separator"sv)
        return "AXSplitter"sv;
    return "AXGroup"sv;
}

// The landmark a role marks, as AppKit's subrole names it.
static Optional<StringView> appkit_accessibility_subrole(StringView role)
{
    if (role == "navigation"sv)
        return "AXLandmarkNavigation"sv;
    if (role == "main"sv)
        return "AXLandmarkMain"sv;
    if (role == "banner"sv)
        return "AXLandmarkBanner"sv;
    if (role == "complementary"sv)
        return "AXLandmarkComplementary"sv;
    if (role == "contentinfo"sv)
        return "AXLandmarkContentInfo"sv;
    if (role == "search"sv)
        return "AXLandmarkSearch"sv;
    if (role == "region"sv)
        return "AXLandmarkRegion"sv;
    if (role == "dialog"sv || role == "alertdialog"sv)
        return "AXApplicationDialog"sv;
    return {};
}

void Host::accessibility_tree_did_change(Page& page)
{
    if (!page.accessibility_is_wanted || page.accessibility_update_is_scheduled)
        return;
    page.accessibility_update_is_scheduled = true;
    run_after(ACCESSIBILITY_UPDATE_DELAY, [this, page_id = page.id] {
        auto found = m_pages.get(page_id);
        if (!found.has_value())
            return;
        auto& page = **found;
        page.accessibility_update_is_scheduled = false;

        JsonArray nodes;
        Vector<i64> pending;
        if (auto const* root = page.accessibility.root())
            pending.append(root->id);
        // NB: The tree manager holds a tree: every node is under the root once, however the page arranged it.
        while (!pending.is_empty()) {
            auto const* node = page.accessibility.node(pending.take_last());
            if (!node)
                continue;
            JsonObject entry;
            entry.set("id"sv, node->id);
            entry.set("role"sv, appkit_accessibility_role(node->role));
            // AppKit has no words of its own for a page's root, which says what WebKit's does.
            if (node->role == "document"sv)
                entry.set("roleDescription"sv, "HTML content"sv);
            if (auto subrole = appkit_accessibility_subrole(node->role); subrole.has_value())
                entry.set("subrole"sv, *subrole);
            // A run of text is its own value, as AppKit's static text is. Every other node is named by its label.
            if (node->role == "text leaf"sv) {
                entry.set("value"sv, node->name);
            } else {
                if (!node->name.is_empty())
                    entry.set("label"sv, node->name);
                if (node->role == "heading"sv && node->heading_level > 0)
                    entry.set("value"sv, node->heading_level);
                else if (node->value_numeric.has_value())
                    entry.set("value"sv, *node->value_numeric);
                else if (!node->value.is_empty())
                    entry.set("value"sv, node->value);
            }
            if (!node->description.is_empty())
                entry.set("help"sv, node->description);
            if (!node->url.is_empty())
                entry.set("url"sv, node->url);
            if (node->is_focused)
                entry.set("focused"sv, true);
            if (node->is_disabled)
                entry.set("disabled"sv, true);
            // NB: The engine measures in CSS pixels, which are the view's points at the zoom the page has.
            JsonArray frame;
            frame.must_append(node->bounds.x() * page.reported_zoom);
            frame.must_append(node->bounds.y() * page.reported_zoom);
            frame.must_append(node->bounds.width() * page.reported_zoom);
            frame.must_append(node->bounds.height() * page.reported_zoom);
            entry.set("frame"sv, move(frame));
            JsonArray children;
            for (auto child_id : node->child_ids) {
                children.must_append(child_id);
                pending.append(child_id);
            }
            entry.set("children"sv, move(children));
            nodes.must_append(move(entry));
        }

        JsonObject message;
        message.set("page"sv, page.id);
        if (auto const* root = page.accessibility.root())
            message.set("root"sv, root->id);
        message.set("nodes"sv, move(nodes));
        send("accessibilityTree"sv, move(message));
    });
}

bool Host::show_notification(HubLink& hub, u64 hub_notification_id, u64 view_id, Web::NotificationsAPI::NotificationData const& data, String const& origin)
{
    auto* page = page_for_view(hub, view_id);
    if (!page)
        return false;

    auto id = String::number(m_next_notification_id++);
    m_notifications.set(id, { hub, hub_notification_id, page->id });

    JsonObject notification;
    notification.set("id"sv, id);
    notification.set("origin"sv, origin);
    notification.set("title"sv, data.title.to_utf8());
    notification.set("body"sv, data.body.to_utf8());
    if (!data.tag.is_empty())
        notification.set("tag"sv, data.tag.to_utf8());
    if (!data.icon_url.is_empty())
        notification.set("iconURL"sv, data.icon_url.to_utf8());
    notification.set("isSilent"sv, data.silent.value_or(false));
    JsonObject fields;
    fields.set("notification"sv, move(notification));
    emit_event(*page, "notificationShown"sv, move(fields));
    return true;
}

void Host::page_closed_notification(HubLink& hub, u64 hub_notification_id)
{
    for (auto& entry : m_notifications) {
        if (entry.value.hub.ptr() != &hub || entry.value.hub_notification_id != hub_notification_id)
            continue;
        if (auto page = m_pages.get(entry.value.page); page.has_value()) {
            JsonObject fields;
            fields.set("id"sv, entry.key);
            emit_event(**page, "notificationClosed"sv, move(fields));
        }
        m_notifications.remove(entry.key);
        return;
    }
}

void Host::handle_script_reply(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    auto reply = fields.get_object("reply"sv);
    if (!id.has_value() || !reply.has_value())
        return;
    if (auto error = reply->get_object("error"sv); error.has_value()) {
        page.hub->async_view_embedder_message_reply(page.view_id, *id, {}, error->get_string("message"sv).value_or("The message was refused"_string));
        return;
    }
    auto value_json = "null"_string;
    if (auto value = reply->get_object("value"sv); value.has_value()) {
        if (auto inner = value->get("value"sv); inner.has_value())
            value_json = inner->serialized();
    }
    page.hub->async_view_embedder_message_reply(page.view_id, *id, move(value_json), {});
}

void Host::handle_evaluate(Page& page, JsonObject const& fields)
{
    auto id = fields.get_u64("id"sv);
    auto request = fields.get_object("request"sv);
    if (!id.has_value() || !request.has_value())
        return;
    page.hub->async_view_evaluate_script(page.view_id, *id, request->get_string("source"sv).value_or({}), world_name_from_contract(request->get_object("world"sv)), request->get_bool("userGesture"sv).value_or(false));
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
    run_after(FIRST_FRAME_WAIT_LIMIT, [this, page_id = page.id, snapshot_id = *id] {
        auto page = m_pages.get(page_id);
        if (page.has_value() && (*page)->snapshots_awaiting_frame.remove_first_matching([&](auto waiting) { return waiting == snapshot_id; }))
            take_snapshot(**page, snapshot_id);
    });
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
    case PendingRequest::Kind::OpenURL:
        break;
    case PendingRequest::Kind::Download: {
        auto* download = staged_download(pending->answer_id);
        if (!download)
            break;
        Optional<ByteString> destination;
        if (auto save_to = answer->get_object("saveTo"sv); save_to.has_value()) {
            if (auto url = URL::Parser::basic_parse(save_to->get_string("url"sv).value_or({})); url.has_value() && url->scheme() == "file"sv)
                destination = url->file_path();
        }
        if (!destination.has_value()) {
            // NB: The Hub reports the cancelled download, which then tells Refrax and removes the staged file.
            hub.async_cancel_download(download->hub_download_id);
            break;
        }
        download->destination = destination.release_value();
        place_download_if_ready(*download);
        break;
    }
    case PendingRequest::Kind::Permission:
        // NB: Refrax keeps every permission decision itself, so the engine is only ever told about this one request,
        //     and keeps no grant that Refrax could not take back.
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

    page.hub->async_view_enqueue_mouse_event(page.view_id, MOUSE_INPUT_ID_BIT | m_next_input_id++, move(event));
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

    // NB: Only a key held with Control and without Command goes by the system's key bindings, such as ^A for the
    //     start of the line. Every other key does what the engine itself gives it.
    auto follows_system_bindings = (modifiers & Web::UIEvents::Mod_Ctrl) && !(modifiers & Web::UIEvents::Mod_Super);
    if (auto commands = fields.get_array("commands"sv); is_down && follows_system_bindings && commands.has_value()) {
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
