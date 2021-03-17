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

#include <terminal_renderer/Atlas.h>
#include <crispy/size.h>

#include <memory>

namespace terminal::renderer {

struct AtlasTextureInfo {
    std::string atlasName;
    unsigned atlasInstanceId;
    crispy::Size size;
    atlas::Format format;
    atlas::Buffer buffer;
};

/**
 * Terminal render target interface.
 *
 * @see OpenGLRenderer
 */
class RenderTarget
{
  public:
    virtual ~RenderTarget() = default;

    virtual void setRenderSize(int _width, int _height) = 0;
    virtual void setMargin(int _left, int _bottom) = 0;

    virtual atlas::TextureAtlasAllocator& monochromeAtlasAllocator() noexcept = 0;
    virtual atlas::TextureAtlasAllocator& coloredAtlasAllocator() noexcept = 0;
    virtual atlas::TextureAtlasAllocator& lcdAtlasAllocator() noexcept = 0;

    virtual atlas::CommandListener& textureScheduler() = 0;

    virtual void renderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height,
                                 float _r, float _g, float _b, float _a) = 0;

    virtual void execute() = 0;

    virtual void clearCache() = 0;

    virtual std::optional<AtlasTextureInfo> readAtlas(atlas::TextureAtlasAllocator const& _allocator, unsigned _instanceId) = 0;
};

class Renderable {
  public:
    virtual ~Renderable() = default;

    virtual void clearCache() {}
    virtual void setRenderTarget(RenderTarget& _renderTarget) { renderTarget_ = &_renderTarget; }
    RenderTarget& renderTarget() { return *renderTarget_; }

    atlas::TextureAtlasAllocator& monochromeAtlasAllocator() noexcept { return renderTarget_->monochromeAtlasAllocator(); }
    atlas::TextureAtlasAllocator& coloredAtlasAllocator() noexcept { return renderTarget_->coloredAtlasAllocator(); }
    atlas::TextureAtlasAllocator& lcdAtlasAllocator() noexcept { return renderTarget_->lcdAtlasAllocator(); }

    atlas::CommandListener& textureScheduler() { return renderTarget_->textureScheduler(); }

  protected:
    RenderTarget* renderTarget_ = nullptr;
};

} // end namespace
