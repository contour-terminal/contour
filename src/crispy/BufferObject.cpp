// SPDX-License-Identifier: Apache-2.0
#include "BufferObject.h"

#include <crispy/BufferObject.h>

namespace crispy
{

template class buffer_object<char>;
template class BufferFragment<char>;
template class buffer_object_pool<char>;

} // namespace crispy
