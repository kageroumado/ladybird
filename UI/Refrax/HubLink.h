/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <Hub/HubClientEndpoint.h>
#include <Hub/HubServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>

namespace Refrax {

class Host;

// Answers every message of the Hub with nothing, so that HubLink only writes the handlers it acts on.
class HubLinkBase
    : public IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>
    , public HubClientEndpoint {
protected:
    explicit HubLinkBase(NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionToServer<HubClientEndpoint, HubServerEndpoint>(*this, move(transport))
    {
    }

#include <UI/Refrax/HubClientNoOps.inc>
};

// The host's connection to one Hub. A Hub serves one profile, so the host runs a Hub for each profile that Refrax has
// pages in, and each page belongs to the link of its profile.
class HubLink final : public HubLinkBase {
    C_OBJECT_ABSTRACT(HubLink)

public:
    // Starts a Hub with the arguments given and waits for it to initialize.
    static ErrorOr<NonnullRefPtr<HubLink>> launch(Host&, String profile_key, Vector<ByteString> arguments, Hub::UIInfo const&);

    String const& profile_key() const { return m_profile_key; }
    pid_t hub_pid() const { return m_hub_pid; }

    // The Hub ends once its connection closes.
    void disconnect();

private:
    HubLink(Host&, String profile_key, NonnullOwnPtr<IPC::Transport>);

    virtual void die() override;

    virtual void view_state_changed(u64 view_id, Hub::ViewState state) override;
    virtual void view_navigation_load_start(u64 view_id, URL::URL url) override;
    virtual void view_navigation_load_finish(u64 view_id, URL::URL url) override;
    virtual void view_top_level_navigation_commit(u64 view_id) override;
    virtual void view_navigation_response(u64 view_id, Vector<URL::URL> url_list, Optional<u16> status, Optional<String> network_error) override;
    virtual void view_url_change(u64 view_id, URL::URL url) override;
    virtual void view_title_change(u64 view_id, Utf16String title) override;
    virtual void view_loading_state_change(u64 view_id, bool is_loading) override;
    virtual void view_link_hover(u64 view_id, URL::URL url) override;
    virtual void view_link_unhover(u64 view_id) override;
    virtual void view_find_in_page(u64 view_id, size_t current_match_index, Optional<size_t> total_match_count) override;
    virtual void view_web_content_crashed(u64 view_id, WebView::ViewImplementation::WebContentCrashReason reason) override;
    virtual void view_tab_indicator_change(u64 view_id, WebView::TabIndicator indicator, bool visible) override;
    virtual void view_cursor_change(u64 view_id, Gfx::Cursor cursor) override;

    virtual void view_set_remote_layer(u64 view_id, Compositing::RemoteLayerHostHandle host_handle) override;
    virtual void view_did_present_frame_in_remote_layer(u64 view_id, Gfx::IntSize size, double device_pixel_ratio) override;
    virtual void view_present_backing_store(u64 view_id, u64 page_id, i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio) override;
    virtual void view_did_take_frame_snapshot(u64 view_id, u64 request_id, Optional<Gfx::ShareableBitmap> snapshot) override;
    virtual void view_did_evaluate_script(u64 view_id, u64 request_id, Optional<String> result_json, Optional<String> error) override;

    virtual void view_finish_handling_key_event(u64 view_id, u64 input_id, Web::KeyEvent event) override;
    virtual void view_release_input(u64 view_id, u64 input_id) override;

    virtual void view_request_alert(u64 view_id, Utf16String message) override;
    virtual void view_request_confirm(u64 view_id, Utf16String message) override;
    virtual void view_request_prompt(u64 view_id, Utf16String message, Utf16String default_value) override;
    virtual void view_request_before_unload(u64 view_id, u64 answer_id, String source) override;
    virtual void view_request_dismiss_dialog(u64 view_id) override;
    virtual void view_request_navigation(u64 view_id, u64 answer_id, URL::URL url, bool is_form_submission, Web::HTML::UserNavigationInvolvement user_involvement, Optional<URL::Origin> initiator_origin) override;
    virtual void view_request_permission(u64 view_id, u64 answer_id, WebView::PermissionPromptRequest request) override;

    virtual void view_selected_text(u64 view_id, u64 request_id, Optional<ByteString> text) override;
    virtual void view_open_new_view(u64 view_id, u64 new_view_id, Hub::ViewState new_view_state, Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, bool has_user_activation) override;
    virtual void app_open_url_in_new_tab(URL::URL url, Web::HTML::ActivateTab activate_tab, Optional<u64> source_view_id, WebView::IsPrivate is_private) override;
    virtual void download_added(Hub::DownloadState download) override;
    virtual void download_updated(Hub::DownloadState download) override;
    virtual void download_started_by_view(u64 download_id, u64 view_id) override;
    virtual void app_set_clipboard_text(String text, WebView::Application::ClipboardType type) override;
    virtual void app_read_clipboard_text(u64 request_id, WebView::Application::ClipboardType type) override;

    Host& m_host;
    String m_profile_key;
    pid_t m_hub_pid { -1 };
    bool m_did_initialize { false };
    bool m_disconnected { false };
};

}
