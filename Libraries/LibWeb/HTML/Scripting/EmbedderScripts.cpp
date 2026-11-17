/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/EmbedderScripts.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(EmbedderWorlds);

// A message a script posted to the embedder, whose answer settles the promise the script holds.
struct PendingEmbedderMessage {
    GC::Root<WebIDL::Promise> promise;
    GC::Root<JS::Realm> realm;
    // NB: Only compared, to forget the messages of a window whose worlds are gone.
    Window const* window { nullptr };
};

static HashMap<u64, PendingEmbedderMessage>& pending_embedder_messages()
{
    // NB: Never destroyed, as its roots must not outlive the heap they are of.
    static auto& messages = *new HashMap<u64, PendingEmbedderMessage>;
    return messages;
}

static u64 s_next_embedder_message_id = 1;

Vector<EmbedderScript> parse_embedder_scripts(JsonValue const& json)
{
    Vector<EmbedderScript> scripts;
    if (!json.is_array())
        return scripts;

    auto strings = [](JsonObject const& object, StringView key) {
        Vector<String> values;
        if (auto array = object.get_array(key); array.has_value()) {
            array->for_each([&](JsonValue const& value) {
                if (value.is_string())
                    values.append(value.as_string());
            });
        }
        return values;
    };

    json.as_array().for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto const& object = value.as_object();
        auto source = object.get_string("source"sv);
        if (!source.has_value())
            return;

        EmbedderScript script;
        script.id = object.get_string("id"sv).value_or({});
        script.source = Utf16String::from_utf8(*source);
        script.injection_time = object.get_string("injectionTime"sv).value_or({}) == "documentEnd"sv
            ? EmbedderScript::InjectionTime::DocumentEnd
            : EmbedderScript::InjectionTime::DocumentStart;
        if (auto world = object.get_object("world"sv); world.has_value()) {
            if (auto isolated = world->get_object("isolated"sv); isolated.has_value()) {
                if (auto name = isolated->get_string("name"sv); name.has_value() && !name->is_empty())
                    script.world = *name;
            }
        }
        script.main_frame_only = object.get_bool("mainFrameOnly"sv).value_or(false);
        script.matches = strings(object, "matches"sv);
        script.excludes = strings(object, "excludes"sv);
        script.channels = strings(object, "channels"sv);
        scripts.append(move(script));
    });
    return scripts;
}

// Whether the text matches a pattern in which "*" stands for any run of characters.
static bool matches_glob(StringView text, StringView pattern)
{
    size_t text_index = 0;
    size_t pattern_index = 0;
    Optional<size_t> star_index;
    size_t text_index_after_star = 0;
    while (text_index < text.length()) {
        if (pattern_index < pattern.length() && pattern[pattern_index] == '*') {
            star_index = pattern_index++;
            text_index_after_star = text_index;
        } else if (pattern_index < pattern.length() && pattern[pattern_index] == text[text_index]) {
            ++pattern_index;
            ++text_index;
        } else if (star_index.has_value()) {
            pattern_index = *star_index + 1;
            text_index = ++text_index_after_star;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.length() && pattern[pattern_index] == '*')
        ++pattern_index;
    return pattern_index == pattern.length();
}

// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Match_patterns
bool url_matches_pattern(URL::URL const& url, StringView pattern)
{
    auto const& scheme = url.scheme();
    if (pattern == "<all_urls>"sv)
        return scheme.is_one_of("http"sv, "https"sv, "file"sv, "ws"sv, "wss"sv, "ftp"sv, "data"sv);

    auto scheme_end = pattern.find("://"sv);
    if (!scheme_end.has_value())
        return false;
    auto pattern_scheme = pattern.substring_view(0, *scheme_end);
    auto rest = pattern.substring_view(*scheme_end + 3);
    auto path_start = rest.find('/');
    if (!path_start.has_value())
        return false;
    auto pattern_host = rest.substring_view(0, *path_start);
    auto pattern_path = rest.substring_view(*path_start);

    if (pattern_scheme == "*"sv) {
        if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv))
            return false;
    } else if (pattern_scheme != scheme) {
        return false;
    }

    // NB: A file: URL has no host, and its pattern has none either.
    auto host = url.host().has_value() ? url.serialized_host().to_byte_string() : ByteString {};
    if (pattern_host == "*"sv) {
        // Any host.
    } else if (pattern_host.starts_with("*."sv)) {
        auto suffix = pattern_host.substring_view(2);
        if (host != suffix && !host.ends_with(ByteString::formatted(".{}", suffix)))
            return false;
    } else if (pattern_host != host) {
        return false;
    }

    auto path = url.serialize_path().to_byte_string();
    if (auto const& query = url.query(); query.has_value())
        path = ByteString::formatted("{}?{}", path, *query);
    return matches_glob(path, pattern_path);
}

static bool script_matches(EmbedderScript const& script, URL::URL const& url)
{
    for (auto const& pattern : script.excludes) {
        if (url_matches_pattern(url, pattern))
            return false;
    }
    if (script.matches.is_empty())
        return true;
    for (auto const& pattern : script.matches) {
        if (url_matches_pattern(url, pattern))
            return true;
    }
    return false;
}

// Whether the embedder's script runs in the window's document, whenever it is due.
static bool script_applies_to(EmbedderScript const& script, Window& window)
{
    auto& document = window.associated_document();
    auto navigable = document.navigable();
    if (!navigable)
        return false;
    if (script.main_frame_only && navigable->parent())
        return false;
    return script_matches(script, document.url());
}

// The channels the embedder grants to a world in the window's document: those of the world's scripts that run in that
// document, and of no others. The world then finds them under window.webkit.messageHandlers.
static Vector<String> channels_of_world(Window& window, Optional<String> const& world)
{
    Vector<String> channels;
    for (auto const& script : window.page().embedder_scripts()) {
        if (script.world != world || !script_applies_to(script, window))
            continue;
        for (auto const& channel : script.channels) {
            if (!channels.contains_slow(channel))
                channels.append(channel);
        }
    }
    return channels;
}

// Defines window.webkit.messageHandlers.<channel>.postMessage(body) for each channel, in the realm of one world.
// postMessage() returns a promise that the embedder's answer settles.
static void install_message_handlers(JS::Realm& realm, Optional<String> const& world, Vector<String> const& channels)
{
    if (channels.is_empty())
        return;
    auto& vm = realm.vm();

    auto handlers = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto const& channel : channels) {
        // NB: The function names its world and channel itself. Nothing a script passes can name another.
        auto post_message = JS::NativeFunction::create(
            realm,
            GC::create_function(GC::Heap::the(), GC::captures(world, channel), [](auto& world, auto& channel, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto& realm = *vm.current_realm();
                auto* window = window_from_global_object(realm.global_object());
                if (!window)
                    return vm.throw_completion<JS::TypeError>(u"The embedder's channels belong to a window"sv);

                auto body = TRY(JS::JSONObject::stringify_impl(vm, vm.argument(0), JS::js_undefined(), JS::js_undefined()));
                auto promise = WebIDL::create_promise(realm);

                auto message_id = s_next_embedder_message_id++;
                pending_embedder_messages().set(message_id, { GC::make_root(promise), GC::make_root(realm), window });

                auto& document = window->associated_document();
                document.page().client().page_did_post_embedder_message(message_id, world, channel, body.has_value() ? body->to_utf8() : "null"_string, document.id());
                return promise->promise();
            }),
            1, Utf16FlyString::from_utf8_without_validation("postMessage"sv),
            // NB: No script is running as a world is made, so the function is told its realm.
            GC::Ptr<JS::Realm> { realm });

        auto handler = JS::Object::create(realm, realm.intrinsics().object_prototype());
        handler->define_direct_property(Utf16FlyString::from_utf8_without_validation("postMessage"sv), post_message, JS::Attribute::Enumerable);
        handlers->define_direct_property(Utf16FlyString::from_utf8(channel), handler, JS::Attribute::Enumerable);
    }

    auto webkit = JS::Object::create(realm, realm.intrinsics().object_prototype());
    webkit->define_direct_property(Utf16FlyString::from_utf8_without_validation("messageHandlers"sv), handlers, JS::Attribute::Enumerable);
    realm.global_object().define_direct_property(Utf16FlyString::from_utf8_without_validation("webkit"sv), webkit, JS::Attribute::Enumerable);
    (void)vm;
}

EmbedderWorlds::~EmbedderWorlds() = default;

void EmbedderWorlds::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& world : m_worlds) {
        visitor.visit(world.value.realm);
        visitor.visit(world.value.wrapper_world);
        visitor.visit(world.value.window_proxy);
        if (world.value.execution_context)
            world.value.execution_context->visit_edges(visitor);
    }
}

JS::Realm& EmbedderWorlds::ensure_world(Window& window, String const& name)
{
    if (auto world = m_worlds.get(name); world.has_value())
        return *world->realm;

    auto& vm = Bindings::main_thread_vm();
    auto& principal_realm = window.principal_realm();
    auto wrapper_world = GC::Heap::the().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);
    auto browsing_context = window.browsing_context();

    GC::Ptr<JS::HostObject> global;
    GC::Ptr<WindowProxy> window_proxy;
    auto execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            // The world's global object is a wrapper of its own around the window that the page has.
            global = Bindings::create_global_object_wrapper(realm, GC::Ref { window });
            return *global;
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            // The world's global this is a WindowProxy of its own, as the page's is the browsing context's.
            if (browsing_context) {
                window_proxy = browsing_context->window_proxy_for(*wrapper_world, realm);
                return window_proxy->object();
            }
            return *global;
        });

    auto& realm = *execution_context->realm;
    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    realm.set_host_defined(Bindings::create_host_defined_for_embedder_world(intrinsics, *wrapper_world, principal_realm));
    Bindings::cache_global_object_wrapper(realm);
    MUST(Bindings::initialize_window_web_interfaces(window, realm));

    install_message_handlers(realm, name, channels_of_world(window, name));

    m_worlds.set(name, World { move(execution_context), realm, wrapper_world, window_proxy });
    return realm;
}

void EmbedderWorlds::detach()
{
    for (auto& world : m_worlds)
        world.value.wrapper_world->detach();
    m_worlds.clear();
    m_page_world_has_channels = false;
}

JS::Realm& realm_for_embedder_world(Window& window, Optional<String> const& world)
{
    if (!world.has_value())
        return window.principal_realm();
    return window.embedder_worlds().ensure_world(window, *world);
}

JS::Completion run_script_of_embedder(Window& window, Optional<String> const& world, Utf16String source, StringView filename)
{
    auto& realm = realm_for_embedder_world(window, world);
    auto& vm = realm.vm();

    // NB: The embedder's scripts run whether or not the page's own may, so nothing here asks whether scripting is on.
    TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto script = JS::Script::parse(source.utf16_view(), realm, filename);
    if (script.is_error())
        return JS::throw_completion(JS::SyntaxError::create(realm, script.error().first().to_utf16_string()));
    return vm.run(*script.value());
}

// The URL that names the document's site. A document is its own site's when its URL names a host. One whose URL names
// none is the site's that its origin names (a blob: URL's, a frame's initial about:blank), or else the site's that
// embeds it (a sandboxed frame, an about:srcdoc or data: URL's document).
static URL::URL site_url_of(DOM::Document const& document)
{
    for (GC::Ptr<DOM::Document const> current = document; current;) {
        if (current->url().host().has_value())
            return current->url();
        if (auto const& origin = current->origin(); !origin.is_opaque()) {
            if (auto origin_url = URL::Parser::basic_parse(origin.serialize()); origin_url.has_value() && origin_url->host().has_value())
                return origin_url.release_value();
        }
        auto navigable = current->navigable();
        auto container = navigable ? navigable->container() : nullptr;
        current = container ? &container->document() : nullptr;
    }
    return document.url();
}

bool embedder_allows_page_scripts(DOM::Document const& document)
{
    return document.page().page_scripts_may_run_in(site_url_of(document));
}

bool embedder_allows_autoplay_with_sound(DOM::Document const& document)
{
    return document.page().autoplay_with_sound_allowed_in(site_url_of(document));
}

void run_embedder_scripts(DOM::Document& document, EmbedderScript::InjectionTime injection_time)
{
    auto& page = document.page();
    if (page.embedder_scripts().is_empty())
        return;
    auto window = document.window();
    auto navigable = document.navigable();
    if (!window || !navigable)
        return;

    // NB: A match pattern names a document that came from somewhere, which a frame's initial empty one did not.
    auto const& url = document.url();
    if (!url.scheme().is_one_of("http"sv, "https"sv, "file"sv))
        return;
    auto is_main_frame = !navigable->parent();

    // NB: A document start is told once, by the parser as it makes the root element. A document that reaches its end
    //     without one, as when it has no root element to make, has its start then.
    if (injection_time == EmbedderScript::InjectionTime::DocumentStart) {
        if (document.embedder_document_start_scripts_ran())
            return;
        document.set_embedder_document_start_scripts_ran();
    } else if (!document.embedder_document_start_scripts_ran()) {
        run_embedder_scripts(document, EmbedderScript::InjectionTime::DocumentStart);
    }

    // The page's own world gets its channels before any script of the document runs.
    if (injection_time == EmbedderScript::InjectionTime::DocumentStart && !window->embedder_worlds().page_world_has_channels()) {
        if (auto channels = channels_of_world(*window, {}); !channels.is_empty()) {
            install_message_handlers(window->principal_realm(), {}, channels);
            window->embedder_worlds().set_page_world_has_channels();
        }
    }

    // NB: A script can change the page's scripts by way of the embedder, so the ones due now are copied first.
    Vector<EmbedderScript> due;
    for (auto const& script : page.embedder_scripts()) {
        if (script.injection_time != injection_time)
            continue;
        if (script.main_frame_only && !is_main_frame)
            continue;
        if (!script_matches(script, url))
            continue;
        due.append(script);
    }

    for (auto const& script : due) {
        auto completion = run_script_of_embedder(*window, script.world, script.source, script.id.bytes_as_string_view());
        if (completion.is_abrupt())
            report_an_exception(realm_for_embedder_world(*window, script.world).global_object(), completion.value());
    }
}

void settle_embedder_message(u64 message_id, Optional<String> const& value_json, Optional<String> const& error)
{
    auto pending = pending_embedder_messages().take(message_id);
    if (!pending.has_value())
        return;
    auto& realm = *pending->realm;
    auto& promise = *pending->promise;

    TemporaryExecutionContext execution_context { realm, TemporaryExecutionContext::CallbacksEnabled::Yes };
    if (error.has_value()) {
        WebIDL::reject_promise(realm, promise, JS::Error::create(realm, Utf16String::from_utf8(*error)));
        return;
    }
    auto value_text = Utf16String::from_utf8(value_json.value_or("null"_string));
    auto value = Infra::parse_json_string_to_javascript_value(realm, value_text.utf16_view());
    WebIDL::resolve_promise(realm, promise, value.is_error() ? JS::js_null() : value.release_value());
}

void forget_embedder_messages_of(Window const& window)
{
    pending_embedder_messages().remove_all_matching([&](auto, PendingEmbedderMessage const& message) {
        return message.window == &window;
    });
}

}
