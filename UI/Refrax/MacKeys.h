/*
 * Copyright (c) 2026, Mado Kagerou <mail@kagerou.glass>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibWebCommon/Page/TextEditingCommand.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Refrax {

// The key that a macOS virtual key code names when the key has no character of its own, such as an arrow or a function
// key. The client sends the codes of NSEvent as they are.
constexpr Web::UIEvents::KeyCode key_code_from_mac_virtual_key(u16 virtual_key)
{
    using enum Web::UIEvents::KeyCode;
    switch (virtual_key) {
    case 0x24: // kVK_Return
    case 0x4C: // kVK_ANSI_KeypadEnter
        return Key_Return;
    case 0x30:
        return Key_Tab;
    case 0x31:
        return Key_Space;
    case 0x33:
        return Key_Backspace;
    case 0x35:
        return Key_Escape;
    case 0x37:
        return Key_LeftSuper;
    case 0x36:
        return Key_RightSuper;
    case 0x38:
        return Key_LeftShift;
    case 0x3C:
        return Key_RightShift;
    case 0x39:
        return Key_CapsLock;
    case 0x3A:
        return Key_LeftAlt;
    case 0x3D:
        return Key_RightAlt;
    case 0x3B:
        return Key_LeftControl;
    case 0x3E:
        return Key_RightControl;
    case 0x72: // kVK_Help, where a PC keyboard has Insert
        return Key_Insert;
    case 0x73:
        return Key_Home;
    case 0x74:
        return Key_PageUp;
    case 0x75:
        return Key_Delete;
    case 0x77:
        return Key_End;
    case 0x79:
        return Key_PageDown;
    case 0x7B:
        return Key_Left;
    case 0x7C:
        return Key_Right;
    case 0x7D:
        return Key_Down;
    case 0x7E:
        return Key_Up;
    case 0x7A:
        return Key_F1;
    case 0x78:
        return Key_F2;
    case 0x63:
        return Key_F3;
    case 0x76:
        return Key_F4;
    case 0x60:
        return Key_F5;
    case 0x61:
        return Key_F6;
    case 0x62:
        return Key_F7;
    case 0x64:
        return Key_F8;
    case 0x65:
        return Key_F9;
    case 0x6D:
        return Key_F10;
    case 0x67:
        return Key_F11;
    case 0x6F:
        return Key_F12;
    default:
        return Key_Invalid;
    }
}

// The editing command of a selector that AppKit's key bindings name, such as "moveWordBackwardAndModifySelection:".
inline Optional<Web::TextEditingCommand> text_editing_command_from_selector(StringView selector)
{
    using Type = Web::TextEditingCommand::Type;
    struct Entry {
        StringView selector;
        Type type;
        bool can_extend_selection;
    };
    static constexpr Entry entries[] = {
        { "moveBackward"sv, Type::MoveBackward, true },
        { "moveLeft"sv, Type::MoveBackward, true },
        { "moveForward"sv, Type::MoveForward, true },
        { "moveRight"sv, Type::MoveForward, true },
        { "moveWordBackward"sv, Type::MoveWordBackward, true },
        { "moveWordLeft"sv, Type::MoveWordBackward, true },
        { "moveWordForward"sv, Type::MoveWordForward, true },
        { "moveWordRight"sv, Type::MoveWordForward, true },
        { "moveUp"sv, Type::MoveUp, true },
        { "moveDown"sv, Type::MoveDown, true },
        { "moveToBeginningOfParagraph"sv, Type::MoveToBeginningOfParagraph, true },
        { "moveToEndOfParagraph"sv, Type::MoveToEndOfParagraph, true },
        { "moveToBeginningOfLine"sv, Type::MoveToBeginningOfLine, true },
        { "moveToEndOfLine"sv, Type::MoveToEndOfLine, true },
        { "moveToLeftEndOfLine"sv, Type::MoveToLeftEndOfLine, true },
        { "moveToRightEndOfLine"sv, Type::MoveToRightEndOfLine, true },
        { "pageUp"sv, Type::PageUp, true },
        { "pageDown"sv, Type::PageDown, true },
        { "deleteBackward"sv, Type::DeleteBackward, false },
        { "deleteForward"sv, Type::DeleteForward, false },
        { "deleteWordBackward"sv, Type::DeleteWordBackward, false },
        { "deleteWordForward"sv, Type::DeleteWordForward, false },
        { "deleteToEndOfParagraph"sv, Type::DeleteToEndOfParagraph, false },
    };

    if (!selector.ends_with(':'))
        return {};
    selector = selector.substring_view(0, selector.length() - 1);
    auto extends_selection = selector.ends_with("AndModifySelection"sv);
    if (extends_selection)
        selector = selector.substring_view(0, selector.length() - "AndModifySelection"sv.length());

    for (auto const& entry : entries) {
        if (entry.selector != selector)
            continue;
        if (extends_selection && !entry.can_extend_selection)
            return {};
        return Web::TextEditingCommand { .type = entry.type, .extend_selection = extends_selection };
    }
    return {};
}

}
