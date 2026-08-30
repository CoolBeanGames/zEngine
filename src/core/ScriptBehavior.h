#pragma once
#include "GameObject.h"
#include <stdexcept>

namespace zengine
{
    // Project-relative asset reference. Runtime instances belong to a later scripting bridge.
    class ScriptBehavior final : public Behavior
    {
    public:
        ScriptBehavior(GameObject& owner, std::string asset) : Behavior(owner), asset_(std::move(asset))
        {
            if (asset_.empty()) throw std::invalid_argument("Script asset reference cannot be empty.");
        }
        const std::string& Asset() const noexcept { return asset_; }
    private:
        std::string asset_;
    };
}
