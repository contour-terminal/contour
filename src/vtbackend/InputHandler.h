// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/InputGenerator.h>

namespace terminal
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
    virtual bool sendKeyPressEvent(Key key, Modifier modifier) = 0;
    virtual bool sendCharPressEvent(char32_t codepoint, Modifier modifier) = 0;
};

} // namespace terminal
