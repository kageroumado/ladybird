/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibMain/Main.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <UI/Refrax/Host.h>
#include <UI/Refrax/Wire.h>

// The Refrax engine host. Refrax's engine client starts it with one end of a socket pair, and it runs until that
// socket closes.
ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    int wire_fd = -1;
    Core::ArgsParser args_parser;
    args_parser.set_general_help("Shows Ladybird's pages in Refrax. Started by Refrax's engine client.");
    args_parser.add_option(wire_fd, "The socket that the engine client speaks on", "wire-fd", 0, "fd");
    args_parser.parse(arguments);

    if (wire_fd < 0) {
        warnln("RefraxLadybirdHost is started by Refrax, with --wire-fd");
        return 2;
    }

    WebView::platform_init();
    Core::EventLoop event_loop;

    auto wire = TRY(Refrax::Wire::adopt(wire_fd));
    Refrax::Host host { move(wire) };

    return event_loop.exec();
}
