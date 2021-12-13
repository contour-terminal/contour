/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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
#include <terminal_renderer/Atlas.h>

#include <crispy/logstore.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <type_traits>
#include <vector>

using crispy::Point;

using namespace std;

namespace terminal::renderer::atlas
{

TextureAtlasAllocator::TextureAtlasAllocator(AtlasBackend& _atlasBackend,
                                             ImageSize _atlasTextureSize,
                                             int _maxInstances,
                                             Format _format,
                                             int _user,
                                             string _name):
    atlasBackend_ { _atlasBackend },
    maxInstances_ { static_cast<size_t>(_maxInstances) },
    size_ { _atlasTextureSize },
    format_ { _format },
    user_ { _user },
    name_ { std::move(_name) }
{
    getOrCreateNewAtlas();
}

TextureAtlasAllocator::~TextureAtlasAllocator()
{
    for (auto&& atlasID: atlasIDs_)
        atlasBackend_.destroyAtlas(atlasID);
    for (auto&& atlasID: unusedAtlasIDs_)
        atlasBackend_.destroyAtlas(atlasID);
}

void TextureAtlasAllocator::clear()
{
    maxTextureHeightInCurrentRow_ = 0;
    discarded_.clear();

    unusedAtlasIDs_.insert(unusedAtlasIDs_.end(), atlasIDs_.begin(), prev(atlasIDs_.end()));
    atlasIDs_.clear();
    atlasIDs_.push_back(cursor_.atlas);

    cursor_.position.x = 0;
    cursor_.position.y = 0;
}

optional<TextureAtlasAllocator::Cursor> TextureAtlasAllocator::getOffsetAndAdvance(ImageSize _size)
{
    if (!(cursor_.position.x + HorizontalGap + *_size.width < *size_.width))
    {
        cursor_.position.x = 0;
        cursor_.position.y += static_cast<int>(maxTextureHeightInCurrentRow_) + VerticalGap;
        if (!(cursor_.position.y + *_size.height < *size_.height))
        {
            cursor_.position.y = 0;
            maxTextureHeightInCurrentRow_ = 0;

            if (!(atlasIDs_.size() + 1 < maxInstances_))
            {
                cursor_.position.x = *size_.width;
                cursor_.position.y = *size_.height;
                return nullopt;
            }
            getOrCreateNewAtlas();
        }
    }

    auto const result = cursor_;
    cursor_.position.x += unbox<int>(_size.width) + HorizontalGap;
    maxTextureHeightInCurrentRow_ = max(maxTextureHeightInCurrentRow_, *_size.height);
    return result;
}

TextureInfo const* TextureAtlasAllocator::insert(
    ImageSize _bitmapSize, ImageSize _targetSize, Format _format, Buffer _data, int _user)
{
    // check free-map first
    if (auto i = discarded_.find(_bitmapSize); i != end(discarded_))
    {
        std::vector<Cursor>& discardsForGivenSize = i->second;
        if (!discardsForGivenSize.empty())
        {
            TextureInfo const& info =
                appendTextureInfo(_bitmapSize, _targetSize, discardsForGivenSize.back(), _user);

            discardsForGivenSize.pop_back();
            if (discardsForGivenSize.empty())
                discarded_.erase(i);

            atlasBackend_.uploadTexture(UploadTexture { std::ref(info), std::move(_data), _format });

            return &info;
        }
    }

    // fail early if to-be-inserted texture is too large to fit a single page in the whole atlas
    if (_bitmapSize.height > size_.height || _bitmapSize.width > size_.width)
        return nullptr;

    auto const targetOffset = getOffsetAndAdvance(_bitmapSize);
    if (!targetOffset.has_value())
        return nullptr;

    TextureInfo const& info = appendTextureInfo(_bitmapSize, _targetSize, *targetOffset, _user);

    atlasBackend_.uploadTexture(UploadTexture { std::ref(info), std::move(_data), _format });

    // LOGSTORE(AtlasLog)("Insert texture into atlas. {}", info);
    return &info;
}

void TextureAtlasAllocator::release(TextureInfo const& _info)
{
    auto i = std::find_if(begin(textureInfos_), end(textureInfos_), [&](TextureInfo const& ti) -> bool {
        return &ti == &_info;
    });

    if (i != end(textureInfos_))
    {
        std::vector<Cursor>& discardsForGivenSize = discarded_[_info.bitmapSize];
        discardsForGivenSize.emplace_back(Cursor { _info.atlas, _info.offset });
        textureInfos_.erase(i);
    }
}

TextureInfo const& TextureAtlasAllocator::appendTextureInfo(ImageSize _bitmapSize,
                                                            ImageSize _targetSize,
                                                            Cursor _offset,
                                                            int _user)
{
    textureInfos_.emplace_back(
        TextureInfo { _offset.atlas,
                      name_,
                      _offset.position,
                      _bitmapSize,
                      _targetSize,
                      static_cast<float>(_offset.position.x) / unbox<float>(size_.width),
                      static_cast<float>(_offset.position.y) / unbox<float>(size_.height),
                      unbox<float>(_bitmapSize.width) / unbox<float>(size_.width),
                      unbox<float>(_bitmapSize.height) / unbox<float>(size_.height),
                      _user });

    return textureInfos_.back();
}

} // namespace terminal::renderer::atlas
