// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

struct HandledTag
{
};
using Handled = boxed::boxed<bool, HandledTag>;

/**
 * Generic input handler interface.
 *
 * @see ViInputHandler
 * @see Terminal
 */
class InputHandler
{
  public:
    virtual ~InputHandler() = default;
    virtual Handled sendKeyPressEvent(Key key, Modifiers modifiers, KeyboardEventType eventType) = 0;
    virtual Handled sendCharPressEvent(char32_t codepoint,
                                       Modifiers modifiers,
                                       KeyboardEventType eventType) = 0;
};

} // namespace vtbackend
