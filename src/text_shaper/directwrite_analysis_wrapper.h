/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include<string>

#include <dwrite.h>
#include <dwrite_3.h>
#include <wrl/implements.h>

using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::InhibitFtmBase;

namespace text {
    class dwrite_analysis_wrapper : public RuntimeClass<RuntimeClassFlags<ClassicCom | InhibitFtmBase>, IDWriteTextAnalysisSource, IDWriteTextAnalysisSink>
    {
    public:
        dwrite_analysis_wrapper(const std::wstring& _text, const std::wstring& _userLocale) :
            text(_text),
            userLocale(_userLocale)
        {
        }

#pragma region IDWriteTextAnalysisSource
        HRESULT GetTextAtPosition(
            UINT32 textPosition,
            _Outptr_result_buffer_(*textLength) WCHAR const** textString,
            _Out_ UINT32* textLength)
        {
            *textString = nullptr;
            *textLength = 0;

            if (textPosition < text.size())
            {
                *textString = &text.at(textPosition);
                *textLength = text.size() - textPosition;
            }

            return S_OK;
        }

        HRESULT GetTextBeforePosition(
            UINT32 textPosition,
            _Outptr_result_buffer_(*textLength) WCHAR const** textString,
            _Out_ UINT32* textLength)
        {
            *textString = nullptr;
            *textLength = 0;

            if (textPosition > 0 && textPosition <= text.size())
            {
                *textString = text.data();
                *textLength = textPosition;
            }

            return S_OK;
        }

        DWRITE_READING_DIRECTION GetParagraphReadingDirection()
        {
            // TODO: is this always correct?
            return DWRITE_READING_DIRECTION::DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
        }

        HRESULT GetLocaleName(
            UINT32 textPosition,
            _Out_ UINT32* textLength,
            _Outptr_result_z_ WCHAR const** localeName)
        {
            *localeName = userLocale.c_str();
            *textLength = text.size() - textPosition;

            return S_OK;
        }

        HRESULT GetNumberSubstitution(UINT32 textPosition,
            _Out_ UINT32* textLength,
            _COM_Outptr_ IDWriteNumberSubstitution** numberSubstitution)
        {

            *numberSubstitution = nullptr;
            *textLength = text.size() - textPosition;

            return S_OK;
        }
#pragma endregion

#pragma region IDWriteTextAnalysisSink
        HRESULT SetScriptAnalysis(UINT32 textPosition, UINT32 textLength, _In_ DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis)
        {
            script = *scriptAnalysis;
            return S_OK;
        }

        HRESULT SetLineBreakpoints(UINT32 textPosition,
            UINT32 textLength,
            _In_reads_(textLength) DWRITE_LINE_BREAKPOINT const* lineBreakpoints)
        {
            return S_OK;
        }

        HRESULT SetBidiLevel(UINT32 textPosition,
            UINT32 textLength,
            UINT8 /*explicitLevel*/,
            UINT8 resolvedLevel)
        {
            return S_OK;
        }

        HRESULT SetNumberSubstitution(UINT32 textPosition,
            UINT32 textLength,
            _In_ IDWriteNumberSubstitution* numberSubstitution)
        {
            return S_OK;
        }
#pragma endregion

        DWRITE_SCRIPT_ANALYSIS script;

    private:
        const std::wstring& text;
        const std::wstring& userLocale;
    };
}
