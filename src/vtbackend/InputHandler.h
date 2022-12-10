/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
    virtual bool sendKeyPressEvent(Key _key, Modifier _modifier) = 0;
    virtual bool sendCharPressEvent(char32_t _char, Modifier _modifier) = 0;
};

} // namespace terminal
