/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web {

// The format of the blocking engines an embedder can hand over, as the engine contract's policy names what wrote
// one: the version of adblock-rust that serialized it, which is the only one that reads it.
inline constexpr auto embedder_blocking_engine_format = "adblock-0.13.3;shields-1"sv;

WEBCOMMON_API Optional<String> content_blocking_site(URL::URL const&);
WEBCOMMON_API Optional<String> canonical_content_blocking_host(StringView);

}
