// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The marker `lupdate` collects source sentences by, for headers that are Qt-free yet hold sentences the
/// user reads.

// The marker is spelled as Qt spells it (same parameters, same expansion, so a translation unit that has Qt's
// own sees an identical redefinition) and does nothing here: the sentence is translated by whoever uses it.
#ifndef QT_TRANSLATE_NOOP
    #define QT_TRANSLATE_NOOP(scope, x) x
#endif
