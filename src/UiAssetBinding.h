#pragma once

#include "Renderer.h"
#include "AssetLibrary.h"
#include "ui/UiControl.h"
#include "ui/VideoClip.h"

#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace zengine::ui
{
    // Wires a UiContext's texture / texture-size / video-frame callbacks to the
    // renderer and the project's asset library, with per-path caches. One instance
    // lives for the length of a render loop (player window or editor viewport);
    // call Invalidate() when the device or the project changes.
    class UiAssetBinding
    {
    public:
        UiAssetBinding(Renderer& renderer, std::filesystem::path assetsRoot)
            : renderer_(&renderer), assetsRoot_(std::move(assetsRoot)) {}

        void Bind(UiContext& context)
        {
            context.resolveTexture = [this](std::string_view name) { return Texture(name); };
            context.textureSize = [this](std::string_view name) { return Size(name); };
            context.resolveVideoFrame = [this](std::string_view asset, double time, bool loop)
            { return Frame(asset, time, loop); };
        }

        void Invalidate()
        {
            textures_.clear();
            clips_.clear();
            frames_.clear();
        }

        TextureHandle Texture(std::string_view name)
        {
            const std::string key(name);
            if (key.empty()) return renderer_->WhiteTexture();
            if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
            TextureHandle handle;
            try { handle = renderer_->UploadImage(assetLibrary::Resolve(assetsRoot_, std::filesystem::u8path(key))); }
            catch (...) { handle = renderer_->WhiteTexture(); }
            textures_.emplace(key, handle);
            return handle;
        }

        // Native pixel size, or {0,0} when the image is missing / not yet a real texture.
        Vec2 Size(std::string_view name)
        {
            const auto handle = Texture(name);
            if (!handle || handle == renderer_->WhiteTexture()) return {};
            const auto size = TextureSize(handle);
            return {size.x, size.y};
        }

        TextureHandle Frame(std::string_view asset, double time, bool loop)
        {
            const std::string key(asset);
            if (key.empty()) return renderer_->WhiteTexture();
            auto clipIt = clips_.find(key);
            if (clipIt == clips_.end())
            {
                VideoClip clip;
                try { clip = VideoClip::LoadFile(assetLibrary::Resolve(assetsRoot_, std::filesystem::u8path(key))); }
                catch (...) { clip = {}; }
                clipIt = clips_.emplace(key, std::move(clip)).first;
            }
            const auto& clip = clipIt->second;
            if (!clip.Valid()) return renderer_->WhiteTexture();
            const int frame = clip.FrameAt(time, loop);
            auto& cached = frames_[key];
            if (!cached.second || cached.first != frame)
                cached = {frame, renderer_->UploadTexture(static_cast<std::uint32_t>(clip.Width()),
                                                          static_cast<std::uint32_t>(clip.Height()), clip.Frame(frame))};
            return cached.second;
        }

    private:
        Renderer* renderer_ = nullptr;
        std::filesystem::path assetsRoot_;
        std::map<std::string, TextureHandle> textures_;
        std::map<std::string, VideoClip> clips_;
        std::map<std::string, std::pair<int, TextureHandle>> frames_;
    };
}
