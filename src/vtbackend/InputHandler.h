// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>

namespace vtbackend
{

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
    virtual bool sendKeyPressEvent(Key key, Modifiers modifiers) = 0;
    virtual bool sendCharPressEvent(char32_t codepoint, Modifiers modifiers) = 0;
};

} // namespace vtbackend
