/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A blocking engine that an embedder built and serialized, read as the embedder wrote it. The serialized form is
//! only readable by the version of adblock-rust that wrote it, which is why Cargo.toml pins that version.

use crate::rust_panic::abort_on_panic;
use crate::{ContentBlockerString, as_str, string_to_ffi};
use adblock::engine::Engine;
use adblock::request::Request;
use adblock::resources::Resource;

pub struct EmbedderBlockerEngine {
    engine: Engine,
}

/// What the engine says of one request, as a JSON object:
///   {"block": bool, "matched": bool, "redirect"?: data URL, "rewritten"?: URL, "important": bool, "rule"?: rule, "exception"?: rule}
/// where a rule is adblock-rust's FilterRuleDebugInfo: the filter's text and where its list has it, as far as the
/// engine kept them.
/// A blocked request with `redirect` is answered with that resource. One with `rewritten` loads that URL instead.
fn decision_json(engine: &EmbedderBlockerEngine, request: &Request) -> String {
    let result = engine.engine.check_network_request(request);
    let mut decision = serde_json::Map::new();
    decision.insert("block".into(), result.should_block().into());
    decision.insert("matched".into(), result.filter.is_some().into());
    decision.insert("important".into(), result.important.into());
    if let Some(redirect) = result.redirect {
        decision.insert("redirect".into(), redirect.into());
    }
    if let Some(rewritten) = result.rewritten_url {
        decision.insert("rewritten".into(), rewritten.into());
    }
    if let Some(rule) = result.filter.and_then(|rule| serde_json::to_value(rule).ok()) {
        decision.insert("rule".into(), rule);
    }
    if let Some(exception) = result.exception.and_then(|exception| serde_json::to_value(exception).ok()) {
        decision.insert("exception".into(), exception);
    }
    serde_json::Value::Object(decision).to_string()
}

/// The style rule that does what a procedural or action filter does, where one can: a filter that is one CSS selector
/// and hides what it selects or gives it a style. Every other filter needs the page's procedural runner.
fn style_rule_for_action_filter(filter_json: &str) -> Option<String> {
    let filter: serde_json::Value = serde_json::from_str(filter_json).ok()?;
    let [operator] = filter.get("selector")?.as_array()?.as_slice() else {
        return None;
    };
    if operator.get("type")?.as_str()? != "css-selector" {
        return None;
    }
    let selector = operator.get("arg")?.as_str()?;
    match filter.get("action") {
        None | Some(serde_json::Value::Null) => Some(format!("{selector} {{ display: none !important; }}\n")),
        Some(action) if action.get("type")?.as_str()? == "style" => {
            Some(format!("{selector} {{ {} }}\n", action.get("arg")?.as_str()?))
        }
        Some(_) => None,
    }
}

fn cosmetic_css_for_url(engine: &EmbedderBlockerEngine, url: &str, classes: &[&str], ids: &[&str]) -> String {
    let resources = engine.engine.url_cosmetic_resources(url);
    let mut selectors: Vec<String> = resources.hide_selectors.iter().cloned().collect();
    if !resources.generichide {
        selectors.extend(engine.engine.hidden_class_id_selectors(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        ));
    }
    selectors.sort_unstable();
    selectors.dedup();

    let mut css = String::new();
    for selector in selectors {
        css.push_str(&selector);
        css.push_str(" { display: none !important; }\n");
    }
    let mut style_rules: Vec<String> = resources
        .procedural_actions
        .iter()
        .filter_map(|filter| style_rule_for_action_filter(filter))
        .collect();
    style_rules.sort_unstable();
    for rule in style_rules {
        css.push_str(&rule);
    }
    css
}

/// An engine from the serialized form an embedder wrote and its resources, a JSON array of adblock-rust resources or
/// nothing. C++ frees it with `rust_embedder_blocker_free`. Null where either cannot be read.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_create(
    serialized: ak::Span<'_, u8>,
    resources_json: ak::Span<'_, u8>,
) -> *mut EmbedderBlockerEngine {
    abort_on_panic(|| {
        let mut engine = Engine::default();
        if engine.deserialize(serialized.as_slice()).is_err() {
            return std::ptr::null_mut();
        }
        if !resources_json.as_slice().is_empty() {
            let Ok(resources) = serde_json::from_slice::<Vec<Resource>>(resources_json.as_slice()) else {
                return std::ptr::null_mut();
            };
            engine.use_resources(resources);
        }
        Box::into_raw(Box::new(EmbedderBlockerEngine { engine }))
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_embedder_blocker_create`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_embedder_blocker_free(engine: *mut EmbedderBlockerEngine) {
    abort_on_panic(|| {
        if !engine.is_null() {
            drop(unsafe { Box::from_raw(engine) });
        }
    });
}

/// The engine's decision for a request, as the JSON object `decision_json` describes, or nothing where the request
/// has no URL the engine can read. C++ frees the returned string with `rust_content_blocker_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_decide(
    engine: Option<&EmbedderBlockerEngine>,
    url: ak::Span<'_, u8>,
    source_url: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>,
    method: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        let (Some(engine), Some(url), Some(source_url), Some(request_type), Some(method)) =
            (engine, as_str(url), as_str(source_url), as_str(request_type), as_str(method))
        else {
            return string_to_ffi(String::new());
        };
        let Ok(request) = Request::new(url, source_url, request_type, method) else {
            return string_to_ffi(String::new());
        };
        string_to_ffi(decision_json(engine, &request))
    })
}

/// The Content-Security-Policy directives the engine adds to a document's response, comma-joined, or nothing.
/// C++ frees the returned string with `rust_content_blocker_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_csp_directives(
    engine: Option<&EmbedderBlockerEngine>,
    url: ak::Span<'_, u8>,
    source_url: ak::Span<'_, u8>,
    request_type: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        let (Some(engine), Some(url), Some(source_url), Some(request_type)) =
            (engine, as_str(url), as_str(source_url), as_str(request_type))
        else {
            return string_to_ffi(String::new());
        };
        let Ok(request) = Request::new(url, source_url, request_type, "") else {
            return string_to_ffi(String::new());
        };
        string_to_ffi(engine.engine.get_csp_directives(&request).unwrap_or_default())
    })
}

/// A style sheet that hides what the engine hides in a document at `url` with these classes and ids, each given one
/// to a line. C++ frees the returned string with `rust_content_blocker_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_cosmetic_css(
    engine: Option<&EmbedderBlockerEngine>,
    url: ak::Span<'_, u8>,
    classes: ak::Span<'_, u8>,
    ids: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        let (Some(engine), Some(url), Some(classes), Some(ids)) = (engine, as_str(url), as_str(classes), as_str(ids))
        else {
            return string_to_ffi(String::new());
        };
        let classes: Vec<_> = classes.lines().collect();
        let ids: Vec<_> = ids.lines().collect();
        string_to_ffi(cosmetic_css_for_url(engine, url, &classes, &ids))
    })
}

/// Whether the engine hides anything in a document at `url` for these classes and ids alone.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_has_generic_cosmetic_selectors(
    engine: Option<&EmbedderBlockerEngine>,
    url: ak::Span<'_, u8>,
    classes: ak::Span<'_, u8>,
    ids: ak::Span<'_, u8>,
) -> bool {
    abort_on_panic(|| {
        let (Some(engine), Some(url), Some(classes), Some(ids)) = (engine, as_str(url), as_str(classes), as_str(ids))
        else {
            return false;
        };
        let resources = engine.engine.url_cosmetic_resources(url);
        if resources.generichide {
            return false;
        }
        !engine
            .engine
            .hidden_class_id_selectors(classes.lines(), ids.lines(), &resources.exceptions)
            .is_empty()
    })
}

/// The scripts and procedural filters the engine has for a document at `url`, as a JSON object:
///   {"script": scriptlets to run in the page's world, "procedural": [JSON-encoded procedural filter]}
/// The filters are the ones no style rule can do, which the page's procedural runner applies.
/// C++ frees the returned string with `rust_content_blocker_free_string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_embedder_blocker_page_resources(
    engine: Option<&EmbedderBlockerEngine>,
    url: ak::Span<'_, u8>,
) -> ContentBlockerString {
    abort_on_panic(|| {
        let (Some(engine), Some(url)) = (engine, as_str(url)) else {
            return string_to_ffi(String::new());
        };
        let resources = engine.engine.url_cosmetic_resources(url);
        let mut procedural: Vec<String> = resources
            .procedural_actions
            .into_iter()
            .filter(|filter| style_rule_for_action_filter(filter).is_none())
            .collect();
        procedural.sort_unstable();
        let mut object = serde_json::Map::new();
        object.insert("script".into(), resources.injected_script.into());
        object.insert("procedural".into(), procedural.into());
        string_to_ffi(serde_json::Value::Object(object).to_string())
    })
}
